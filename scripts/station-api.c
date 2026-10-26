/* Small LAN API for the Station Mini. No Python, TLS or periodic polling. */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define PORT 8787
#define TOKEN_FILE "/data/opt/station-api.token"
#define LAUNCH_FILE "/system/vendor/quasar/launch.json"
#define ALICE_BACKUP "/data/local/tmp/backup_q/launch.alice.json"
#define AMIXER "/system/workdir/bin/amixer"

static char token[129];
static int volume_cache = -1;
static time_t volume_checked;
static char mute_cache[8] = "";
static time_t mute_checked;
static void response(int fd, int code, const char *body);

static void read_text(const char *path, char *out, size_t cap)
{
    FILE *f = fopen(path, "r");
    size_t n = 0;
    if (f) {
        n = fread(out, 1, cap - 1, f);
        fclose(f);
    }
    out[n] = 0;
    while (n && isspace((unsigned char)out[n - 1])) out[--n] = 0;
}

static int process_running(const char *needle)
{
    DIR *dir = opendir("/proc");
    struct dirent *ent;
    if (!dir) return 0;
    while ((ent = readdir(dir))) {
        char path[128], name[128];
        if (!isdigit((unsigned char)ent->d_name[0])) continue;
        snprintf(path, sizeof(path), "/proc/%.20s/comm", ent->d_name);
        read_text(path, name, sizeof(name));
        if (!strcmp(name, needle)) { closedir(dir); return 1; }
    }
    closedir(dir);
    return 0;
}

static int alice_mode(void)
{
    char config[4096];
    read_text(LAUNCH_FILE, config, sizeof(config));
    return strstr(config, "quasar/maind") != NULL;
}

static int run_amixer(const char *control, const char *value)
{
    pid_t pid = fork();
    int status;
    if (pid < 0) return -1;
    if (pid == 0) {
        setenv("LD_LIBRARY_PATH", "/system/vendor/quasar/libs", 1);
        if (!freopen("/dev/null", "w", stdout) ||
            !freopen("/dev/null", "w", stderr)) _exit(127);
        execl(AMIXER, "amixer", "-c", "0", "sset", control, value, (char *)NULL);
        _exit(127);
    }
    while (waitpid(pid, &status, 0) < 0) if (errno != EINTR) return -1;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

static void notify_volume(int volume)
{
    const char *tmp = "/tmp/volgest.remote-volume.tmp";
    const char *dst = "/tmp/volgest.remote-volume";
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "%ld %d\n", (long)time(NULL), volume);
    if (fclose(f) == 0) rename(tmp, dst);
    else unlink(tmp);
    f = fopen("/tmp/volgest.volume.tmp", "w");
    if (!f) return;
    fprintf(f, "%d\n", volume);
    if (fclose(f) == 0) rename("/tmp/volgest.volume.tmp", "/tmp/volgest.volume");
    else unlink("/tmp/volgest.volume.tmp");
}

static int read_volume(int fresh)
{
    time_t now = time(NULL);
    if (!fresh && process_running("volgest")) {
        char published[16];
        read_text("/tmp/volgest.volume", published, sizeof(published));
        if (published[0] && strspn(published, "0123456789") == strlen(published) &&
            atoi(published) <= 100) {
            volume_cache = atoi(published);
            volume_checked = now;
            return volume_cache;
        }
    }
    if (!fresh && volume_cache >= 0 && now - volume_checked < 30) return volume_cache;
    FILE *f = popen("LD_LIBRARY_PATH=/system/vendor/quasar/libs " AMIXER
                    " -c 0 sget Master 2>/dev/null", "r");
    char line[256];
    if (!f) return volume_cache;
    while (fgets(line, sizeof(line), f)) {
        char *p = strchr(line, '[');
        if (p && isdigit((unsigned char)p[1])) {
            int v = atoi(p + 1);
            if (v >= 0 && v <= 100 && strchr(p, '%')) volume_cache = v;
        }
    }
    pclose(f);
    volume_checked = now;
    return volume_cache;
}

static void apply_volume(int fd, int volume)
{
    char arg[16], body[64];
    snprintf(arg, sizeof(arg), "%d%%", volume);
    if (run_amixer("Master", arg) != 0) {
        response(fd, 500, "{\"error\":\"amixer failed\"}");
        return;
    }
    volume_cache = -1;
    int actual = read_volume(1);
    if (actual < 0) actual = volume;
    volume_cache = actual;
    volume_checked = time(NULL);
    notify_volume(actual);
    snprintf(body, sizeof(body), "{\"ok\":true,\"volume\":%d}", actual);
    response(fd, 200, body);
}

static const char *read_mute(void)
{
    time_t now = time(NULL);
    if (mute_cache[0] && now - mute_checked < 30) return mute_cache;
    FILE *f = popen("LD_LIBRARY_PATH=/system/vendor/quasar/libs " AMIXER
                    " -c 0 sget 'SPK mute' 2>/dev/null", "r");
    char line[256];
    if (!f) return mute_cache;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "[on]")) strcpy(mute_cache, "on");
        if (strstr(line, "[off]")) strcpy(mute_cache, "off");
    }
    pclose(f);
    mute_checked = now;
    return mute_cache;
}

static void write_all(int fd, const char *buf, size_t len)
{
    while (len) {
        ssize_t n = write(fd, buf, len);
        if (n <= 0) return;
        buf += n; len -= (size_t)n;
    }
}

static void response(int fd, int code, const char *body)
{
    char head[256];
    const char *reason = code == 200 ? "OK" : code == 202 ? "Accepted" :
                         code == 400 ? "Bad Request" : code == 401 ? "Unauthorized" :
                         code == 403 ? "Forbidden" : code == 404 ? "Not Found" :
                         "Internal Server Error";
    int n = snprintf(head, sizeof(head),
        "HTTP/1.1 %d %s\r\nContent-Type: application/json; charset=utf-8\r\n"
        "Content-Length: %zu\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",
        code, reason, strlen(body));
    if (n > 0) { write_all(fd, head, (size_t)n); write_all(fd, body, strlen(body)); }
}

static int header_value(const char *request, const char *key, char *out, size_t cap)
{
    const char *line = strstr(request, "\r\n");
    size_t keylen = strlen(key);
    if (!line) return 0;
    line += 2;
    while (*line && !(line[0] == '\r' && line[1] == '\n')) {
        const char *end = strstr(line, "\r\n");
        if (!end) break;
        if ((size_t)(end - line) > keylen && !strncasecmp(line, key, keylen) &&
            line[keylen] == ':') {
            const char *v = line + keylen + 1;
            while (v < end && (*v == ' ' || *v == '\t')) v++;
            size_t len = (size_t)(end - v);
            if (len >= cap) return 0;
            memcpy(out, v, len); out[len] = 0;
            return 1;
        }
        line = end + 2;
    }
    return 0;
}

static int json_value(const char *body, const char *key, char *out, size_t cap)
{
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(body, pattern);
    if (!p) return 0;
    p = strchr(p + strlen(pattern), ':');
    if (!p) return 0;
    p++;
    while (isspace((unsigned char)*p)) p++;
    int quoted = *p == '"';
    if (quoted) p++;
    size_t n = 0;
    while (*p && n + 1 < cap && (quoted ? *p != '"' : *p != ',' && *p != '}' && !isspace((unsigned char)*p)))
        out[n++] = *p++;
    out[n] = 0;
    return n > 0;
}

static int copy_file(const char *src, const char *dst)
{
    char buf[4096];
    FILE *in = fopen(src, "rb"), *out;
    size_t n;
    if (!in) return -1;
    out = fopen(dst, "wb");
    if (!out) { fclose(in); return -1; }
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
        if (fwrite(buf, 1, n, out) != n) { fclose(in); fclose(out); return -1; }
    fclose(in);
    return fclose(out);
}

static void mode_change_after_reply(int alice)
{
    pid_t pid = fork();
    if (pid != 0) return;
    setsid();
    sleep(2);
    if (alice) {
        if (copy_file(ALICE_BACKUP, LAUNCH_FILE) != 0) _exit(1);
    } else {
        FILE *f = fopen(LAUNCH_FILE, "w");
        if (!f) _exit(1);
        fputs("{\n  \"nodes\": [{\"name\":\"main\",\"root\":\".\",\"tasks\":[]}]\n}\n", f);
        if (fclose(f) != 0) _exit(1);
    }
    sync();
    execl("/sbin/reboot", "reboot", (char *)NULL);
    _exit(1);
}

static void status_response(int fd)
{
    char dist[32], hand[32], ver[128], uptime[64], mem[128], body[1024], volume_text[16];
    int alice = alice_mode();
    int vol = alice ? -1 : read_volume(0);
    long seconds = 0, free_kb = 0;
    read_text("/tmp/volgest.dist", dist, sizeof(dist));
    read_text("/tmp/volgest.hand", hand, sizeof(hand));
    read_text("/system/vendor/quasar/version_full", ver, sizeof(ver));
    read_text("/proc/uptime", uptime, sizeof(uptime));
    read_text("/proc/meminfo", mem, sizeof(mem));
    seconds = atol(uptime);
    if (vol >= 0) snprintf(volume_text, sizeof(volume_text), "%d%%", vol);
    else strcpy(volume_text, "—");
    const char *p = strstr(mem, "MemFree:");
    if (p) free_kb = atol(p + 8);
    snprintf(body, sizeof(body),
        "{\"model\":\"YNDX-0004\",\"version\":\"%.90s\",\"mode\":\"%s\","
        "\"volume\":\"%s\",\"mute\":\"%s\",\"uptime\":\"%ldh %ldm\",\"mem_free\":\"%ld MB\","
        "\"adb\":true,\"ssh\":true,\"volgest\":%s,\"distance\":\"%d mm\","
        "\"hand\":\"%.20s\",\"_ts\":%ld}",
        ver, alice ? "normal" : "maind-free", volume_text, alice ? "—" : read_mute(), seconds / 3600,
        (seconds % 3600) / 60, free_kb / 1024, process_running("volgest") ? "true" : "false",
        atoi(dist), hand, (long)time(NULL));
    response(fd, 200, body);
}

static void handle(int fd)
{
    char req[8192] = {0}, auth[256], length[32], method[16], path[128], value[64];
    size_t used = 0, body_len = 0;
    char *end;
    while (used + 1 < sizeof(req)) {
        ssize_t n = read(fd, req + used, sizeof(req) - used - 1);
        if (n <= 0) return;
        used += (size_t)n; req[used] = 0;
        end = strstr(req, "\r\n\r\n");
        if (!end) continue;
        if (header_value(req, "Content-Length", length, sizeof(length)))
            body_len = (size_t)atoi(length);
        if (body_len > 1024) { response(fd, 400, "{\"error\":\"body too large\"}"); return; }
        if (used >= (size_t)(end + 4 - req) + body_len) break;
    }
    end = strstr(req, "\r\n\r\n");
    if (!end || sscanf(req, "%15s %127s", method, path) != 2) return;
    if (!header_value(req, "X-Auth-Token", auth, sizeof(auth)) || strcmp(auth, token)) {
        response(fd, 401, "{\"error\":\"unauthorized\"}"); return;
    }
    const char *body = end + 4;
    if (!strcmp(method, "GET") && !strcmp(path, "/api/v1/health")) {
        response(fd, 200, "{\"ok\":true}");
    } else if (!strcmp(method, "GET") && !strcmp(path, "/api/v1/status")) {
        status_response(fd);
    } else if (!strcmp(method, "POST") && !strcmp(path, "/api/v1/volume")) {
        if (alice_mode()) { response(fd, 403, "{\"error\":\"alice mode\"}"); return; }
        if (!json_value(body, "value", value, sizeof(value)) || strspn(value, "0123456789") != strlen(value) ||
            atoi(value) > 100) { response(fd, 400, "{\"error\":\"volume must be 0..100\"}"); return; }
        apply_volume(fd, atoi(value));
    } else if (!strcmp(method, "POST") && !strcmp(path, "/api/v1/volume/step")) {
        if (alice_mode()) { response(fd, 403, "{\"error\":\"alice mode\"}"); return; }
        if (!json_value(body, "delta", value, sizeof(value))) {
            response(fd, 400, "{\"error\":\"delta required\"}"); return;
        }
        char *endptr;
        long delta = strtol(value, &endptr, 10);
        if (endptr == value || *endptr || delta < -100 || delta > 100) {
            response(fd, 400, "{\"error\":\"delta must be -100..100\"}"); return;
        }
        int current = read_volume(1);
        if (current < 0) { response(fd, 500, "{\"error\":\"volume unavailable\"}"); return; }
        int next = current + (int)delta;
        if (next < 0) next = 0;
        if (next > 100) next = 100;
        apply_volume(fd, next);
    } else if (!strcmp(method, "POST") && !strcmp(path, "/api/v1/mute")) {
        if (!json_value(body, "value", value, sizeof(value)) ||
            (strcmp(value, "on") && strcmp(value, "off"))) {
            response(fd, 400, "{\"error\":\"mute must be on or off\"}"); return;
        }
        if (run_amixer("SPK mute", value) != 0) { response(fd, 500, "{\"error\":\"amixer failed\"}"); return; }
        strcpy(mute_cache, value); mute_checked = time(NULL);
        response(fd, 200, "{\"ok\":true}");
    } else if (!strcmp(method, "POST") && !strcmp(path, "/api/v1/mode")) {
        if (!json_value(body, "mode", value, sizeof(value)) ||
            (strcmp(value, "alice") && strcmp(value, "custom"))) {
            response(fd, 400, "{\"error\":\"mode must be alice or custom\"}"); return;
        }
        int alice = !strcmp(value, "alice");
        if (alice == alice_mode()) { response(fd, 200, "{\"ok\":true,\"changed\":false}"); return; }
        if (alice && access(ALICE_BACKUP, R_OK) != 0) {
            response(fd, 500, "{\"error\":\"alice backup missing\"}"); return;
        }
        response(fd, 202, "{\"ok\":true,\"rebooting\":true}");
        mode_change_after_reply(alice);
    } else response(fd, 404, "{\"error\":\"not found\"}");
}

int main(void)
{
    int listener, on = 1;
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(PORT),
                               .sin_addr.s_addr = htonl(INADDR_ANY)};
    struct timeval timeout = {.tv_sec = 3};
    read_text(TOKEN_FILE, token, sizeof(token));
    if (strlen(token) < 32) { fprintf(stderr, "station-api: missing token\n"); return 1; }
    signal(SIGPIPE, SIG_IGN);
    listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) return 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) || listen(listener, 8)) {
        perror("station-api listen"); return 1;
    }
    for (;;) {
        int fd = accept(listener, NULL, NULL);
        if (fd < 0) { if (errno != EINTR) sleep(1); continue; }
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        handle(fd);
        close(fd);
    }
}
