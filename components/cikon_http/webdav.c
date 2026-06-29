#include "webdav.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define TAG "cikon:http:webdav"
#define DAV_PREFIX "/dav"
#define DAV_PREFIX_LEN 4
#define DAV_ROOT CONFIG_VFS_LITTLEFS_MOUNT_POINT

/* Max filesystem path: mount point + "/www" + URI */
/* Max filesystem path for a URI-mapped resource */
#define FS_PATH_MAX (sizeof(DAV_ROOT) + CONFIG_HTTPD_MAX_URI_LEN)
/* Child path adds one directory entry name (LittleFS NAME_MAX = 255) */
#define FS_CHILD_PATH_MAX (FS_PATH_MAX + 256)

/* Returns true for macOS metadata files that should be invisible on ESP */
static bool is_macos_junk(const char *path) {
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    return strncmp(name, "._", 2) == 0 || strcmp(name, ".DS_Store") == 0;
}

static void dav_log(const char *method, int status, const char *path, size_t bytes) {
    if (status >= 500)
        ESP_LOGE(TAG, "%s %s %d", method, path, status);
    else if (status >= 400)
        ESP_LOGW(TAG, "%s %s %d", method, path, status);
    else
        ESP_LOGI(TAG, "%s %s %d %zu B", method, path, status, bytes);
}

static bool dav_resolve_path(const char *uri, char *dest, size_t dest_size) {
    if (strncmp(uri, DAV_PREFIX, DAV_PREFIX_LEN) != 0)
        return false;
    if (strstr(uri, "/../") || (strlen(uri) >= 3 && strcmp(uri + strlen(uri) - 3, "/..") == 0))
        return false;
    snprintf(dest, dest_size, DAV_ROOT "%s", uri + DAV_PREFIX_LEN);
    return true;
}

static void propfind_open(httpd_req_t *req) {
    httpd_resp_sendstr_chunk(req, "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                                  "<D:multistatus xmlns:D=\"DAV:\">");
}

static void propfind_entry(httpd_req_t *req, const char *href, bool is_col, size_t size,
                           time_t mtime) {
    char buf[128];
    httpd_resp_sendstr_chunk(req, "<D:response><D:href>");
    httpd_resp_sendstr_chunk(req, href);
    httpd_resp_sendstr_chunk(req, "</D:href><D:propstat><D:prop>");

    const char *name = strrchr(href, '/');
    /* name+1 is empty when href has trailing slash (e.g. "/dav/") — sending
     * an empty chunk would terminate chunked transfer prematurely. Fall back
     * to the full href in that case. */
    const char *display = (name && name[1]) ? name + 1 : href;
    httpd_resp_sendstr_chunk(req, "<D:displayname>");
    httpd_resp_sendstr_chunk(req, display);
    httpd_resp_sendstr_chunk(req, "</D:displayname>");

    if (is_col) {
        httpd_resp_sendstr_chunk(req, "<D:resourcetype><D:collection/></D:resourcetype>");
    } else {
        httpd_resp_sendstr_chunk(req, "<D:resourcetype/>");
        snprintf(buf, sizeof(buf), "<D:getcontentlength>%zu</D:getcontentlength>", size);
        httpd_resp_sendstr_chunk(req, buf);
    }

    struct tm tm_info;
    gmtime_r(&mtime, &tm_info);
    strftime(buf, sizeof(buf), "<D:getlastmodified>%a, %d %b %Y %H:%M:%S GMT</D:getlastmodified>",
             &tm_info);
    httpd_resp_sendstr_chunk(req, buf);

    httpd_resp_sendstr_chunk(
        req, "</D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response>");
}

static void propfind_close(httpd_req_t *req) {
    httpd_resp_sendstr_chunk(req, "</D:multistatus>");
    httpd_resp_send_chunk(req, NULL, 0);
}

/* Drain any request body (PROPFIND may send allprop XML we don't parse) */
static void drain_body(httpd_req_t *req) {
    char buf[64];
    int rem = req->content_len;
    while (rem > 0) {
        int r = httpd_req_recv(req, buf, rem < (int)sizeof(buf) ? rem : (int)sizeof(buf));
        if (r <= 0)
            break;
        rem -= r;
    }
}

static esp_err_t dav_options_handler(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "DAV", "1, 2");
    httpd_resp_set_hdr(req, "Allow",
                       "OPTIONS, GET, HEAD, PUT, DELETE, MKCOL, PROPFIND, MOVE, LOCK, UNLOCK");
    httpd_resp_set_hdr(req, "MS-Author-Via", "DAV");
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t dav_propfind_handler(httpd_req_t *req) {
    char path[FS_PATH_MAX];
    if (!dav_resolve_path(req->uri, path, sizeof(path))) {
        dav_log("PROPFIND", 400, req->uri, 0);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, NULL);
        return ESP_FAIL;
    }

    char depth_str[8] = "0";
    httpd_req_get_hdr_value_str(req, "Depth", depth_str, sizeof(depth_str));
    int depth = atoi(depth_str);

    drain_body(req);

    if (is_macos_junk(path)) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, NULL);
        return ESP_FAIL;
    }

    struct stat st;
    if (stat(path, &st) != 0) {
        dav_log("PROPFIND", 404, path, 0);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, NULL);
        return ESP_FAIL;
    }

    httpd_resp_set_status(req, "207 Multi-Status");
    httpd_resp_set_type(req, "application/xml; charset=utf-8");
    propfind_open(req);

    bool is_dir = S_ISDIR(st.st_mode);
    propfind_entry(req, req->uri, is_dir, is_dir ? 0 : (size_t)st.st_size, st.st_mtime);

    if (depth >= 1 && is_dir) {
        DIR *dir = opendir(path);
        if (dir) {
            char *child_path = malloc(FS_CHILD_PATH_MAX);
            char *child_href = malloc(FS_CHILD_PATH_MAX);
            if (!child_path || !child_href) {
                free(child_path);
                free(child_href);
                closedir(dir);
                propfind_close(req);
                return ESP_OK;
            }

            const char *base = req->uri;
            size_t base_len = strlen(base);
            bool base_slash = base_len > 0 && base[base_len - 1] == '/';
            struct dirent *de;

            while ((de = readdir(dir)) != NULL) {
                if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
                    continue;
                snprintf(child_path, FS_CHILD_PATH_MAX, "%s/%s", path, de->d_name);
                if (base_slash)
                    snprintf(child_href, FS_CHILD_PATH_MAX, "%s%s", base, de->d_name);
                else
                    snprintf(child_href, FS_CHILD_PATH_MAX, "%s/%s", base, de->d_name);

                struct stat cs;
                bool c_is_dir = (de->d_type == DT_DIR);
                size_t c_size = 0;
                time_t c_mtime = 0;
                if (stat(child_path, &cs) == 0) {
                    c_is_dir = S_ISDIR(cs.st_mode);
                    c_size = cs.st_size;
                    c_mtime = cs.st_mtime;
                }
                propfind_entry(req, child_href, c_is_dir, c_size, c_mtime);
            }

            free(child_path);
            free(child_href);
            closedir(dir);
        }
    }

    propfind_close(req);
    return ESP_OK;
}

static esp_err_t dav_get_handler(httpd_req_t *req) {
    char path[FS_PATH_MAX];
    if (!dav_resolve_path(req->uri, path, sizeof(path))) {
        dav_log("GET", 400, req->uri, 0);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, NULL);
        return ESP_FAIL;
    }

    if (is_macos_junk(path)) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, NULL);
        return ESP_FAIL;
    }

    struct stat st;
    if (stat(path, &st) != 0 || S_ISDIR(st.st_mode)) {
        dav_log("GET", 404, path, 0);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, NULL);
        return ESP_FAIL;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        dav_log("GET", 500, path, 0);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/octet-stream");
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        httpd_resp_send_chunk(req, buf, n);
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    dav_log("GET", 200, path, (size_t)st.st_size);
    return ESP_OK;
}

static esp_err_t dav_put_handler(httpd_req_t *req) {
    char path[FS_PATH_MAX];
    if (!dav_resolve_path(req->uri, path, sizeof(path))) {
        dav_log("PUT", 400, req->uri, 0);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, NULL);
        return ESP_FAIL;
    }

    if (is_macos_junk(path)) {
        drain_body(req);
        httpd_resp_set_status(req, "201 Created");
        return httpd_resp_send(req, NULL, 0);
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        dav_log("PUT", 500, path, 0);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot create file");
        return ESP_FAIL;
    }

    char buf[512];
    int rem = req->content_len;
    while (rem > 0) {
        int got = httpd_req_recv(req, buf, rem < (int)sizeof(buf) ? rem : (int)sizeof(buf));
        if (got <= 0) {
            dav_log("PUT", 500, path, 0);
            fclose(f);
            unlink(path);
            return ESP_FAIL;
        }
        fwrite(buf, 1, got, f);
        rem -= got;
    }
    fclose(f);
    dav_log("PUT", 201, path, req->content_len);
    httpd_resp_set_status(req, "201 Created");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t dav_delete_handler(httpd_req_t *req) {
    char path[FS_PATH_MAX];
    if (!dav_resolve_path(req->uri, path, sizeof(path))) {
        dav_log("DELETE", 400, req->uri, 0);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, NULL);
        return ESP_FAIL;
    }

    struct stat st;
    if (stat(path, &st) != 0) {
        dav_log("DELETE", 404, path, 0);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, NULL);
        return ESP_FAIL;
    }

    int rc = S_ISDIR(st.st_mode) ? rmdir(path) : unlink(path);
    if (rc != 0) {
        int status = (errno == ENOTEMPTY) ? 409 : 500;
        dav_log("DELETE", status, path, 0);
        httpd_resp_set_status(req, status == 409 ? "409 Conflict" : "500 Internal Server Error");
        return httpd_resp_send(req, NULL, 0);
    }

    dav_log("DELETE", 204, path, 0);
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t dav_mkcol_handler(httpd_req_t *req) {
    char path[FS_PATH_MAX];
    if (!dav_resolve_path(req->uri, path, sizeof(path))) {
        dav_log("MKCOL", 400, req->uri, 0);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, NULL);
        return ESP_FAIL;
    }

    if (req->content_len > 0) {
        drain_body(req);
        dav_log("MKCOL", 415, path, 0);
        httpd_resp_set_status(req, "415 Unsupported Media Type");
        return httpd_resp_send(req, NULL, 0);
    }

    if (mkdir(path, 0755) != 0) {
        int status = (errno == EEXIST) ? 405 : (errno == ENOENT) ? 409 : 500;
        dav_log("MKCOL", status, path, 0);
        if (status == 405)
            httpd_resp_set_status(req, "405 Method Not Allowed");
        else if (status == 409)
            httpd_resp_set_status(req, "409 Conflict");
        else
            httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_send(req, NULL, 0);
    }

    dav_log("MKCOL", 201, path, 0);
    httpd_resp_set_status(req, "201 Created");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t dav_move_handler(httpd_req_t *req) {
    char src[FS_PATH_MAX];
    if (!dav_resolve_path(req->uri, src, sizeof(src))) {
        dav_log("MOVE", 400, req->uri, 0);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, NULL);
        return ESP_FAIL;
    }

    char dest_hdr[CONFIG_HTTPD_MAX_URI_LEN + 64];
    if (httpd_req_get_hdr_value_str(req, "Destination", dest_hdr, sizeof(dest_hdr)) != ESP_OK) {
        dav_log("MOVE", 400, src, 0);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing Destination");
        return ESP_FAIL;
    }

    /* Destination is a full URL — extract path component starting at /dav */
    const char *dav_part = strstr(dest_hdr, DAV_PREFIX);
    if (!dav_part) {
        dav_log("MOVE", 400, src, 0);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad Destination");
        return ESP_FAIL;
    }

    char dst[FS_PATH_MAX];
    if (!dav_resolve_path(dav_part, dst, sizeof(dst))) {
        dav_log("MOVE", 400, src, 0);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, NULL);
        return ESP_FAIL;
    }

    char overwrite[4] = "T";
    httpd_req_get_hdr_value_str(req, "Overwrite", overwrite, sizeof(overwrite));
    if (overwrite[0] == 'F') {
        struct stat st;
        if (stat(dst, &st) == 0) {
            dav_log("MOVE", 412, src, 0);
            httpd_resp_set_status(req, "412 Precondition Failed");
            return httpd_resp_send(req, NULL, 0);
        }
    }

    if (rename(src, dst) != 0) {
        dav_log("MOVE", 500, src, 0);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Rename failed");
        return ESP_FAIL;
    }

    dav_log("MOVE", 204, src, 0);
    ESP_LOGI(TAG, "  -> %s", dst);
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t dav_lock_handler(httpd_req_t *req) {
    drain_body(req);
    static const char LOCK_BODY[] =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<D:prop xmlns:D=\"DAV:\">"
        "<D:lockdiscovery><D:activelock>"
        "<D:locktype><D:write/></D:locktype>"
        "<D:lockscope><D:exclusive/></D:lockscope>"
        "<D:depth>0</D:depth>"
        "<D:timeout>Second-3600</D:timeout>"
        "<D:locktoken><D:href>urn:uuid:cikon-dav-lock-1</D:href></D:locktoken>"
        "</D:activelock></D:lockdiscovery>"
        "</D:prop>";
    httpd_resp_set_type(req, "application/xml; charset=utf-8");
    httpd_resp_set_hdr(req, "Lock-Token", "<urn:uuid:cikon-dav-lock-1>");
    return httpd_resp_sendstr(req, LOCK_BODY);
}

static esp_err_t dav_unlock_handler(httpd_req_t *req) {
    drain_body(req);
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

/* ------------------------------------------------------------------ init */

void http_webdav_init(httpd_handle_t server) {
    static const httpd_uri_t handlers[] = {
        {.uri = "/dav", .method = HTTP_OPTIONS, .handler = dav_options_handler},
        {.uri = "/dav/*", .method = HTTP_OPTIONS, .handler = dav_options_handler},
        {.uri = "/dav", .method = HTTP_PROPFIND, .handler = dav_propfind_handler},
        {.uri = "/dav/*", .method = HTTP_PROPFIND, .handler = dav_propfind_handler},
        {.uri = "/dav/*", .method = HTTP_GET, .handler = dav_get_handler},
        {.uri = "/dav/*", .method = HTTP_PUT, .handler = dav_put_handler},
        {.uri = "/dav/*", .method = HTTP_DELETE, .handler = dav_delete_handler},
        {.uri = "/dav/*", .method = HTTP_MKCOL, .handler = dav_mkcol_handler},
        {.uri = "/dav/*", .method = HTTP_MOVE, .handler = dav_move_handler},
        {.uri = "/dav/*", .method = HTTP_LOCK, .handler = dav_lock_handler},
        {.uri = "/dav/*", .method = HTTP_UNLOCK, .handler = dav_unlock_handler},
    };
    for (int i = 0; i < (int)(sizeof(handlers) / sizeof(handlers[0])); i++) {
        if (httpd_register_uri_handler(server, &handlers[i]) != ESP_OK)
            ESP_LOGE(TAG, "Failed to register handler %d", i);
    }
    ESP_LOGI(TAG, "WebDAV ready at /dav/");
}
