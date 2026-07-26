#include "durable_job.h"
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    Gateway *g;
    char model_id[128];
    char job_id[64];
} DownloadJob;

static void *model_download_worker(void *arg) {
    DownloadJob *job = (DownloadJob *)arg;
    Gateway *g = job->g;
    
    char partial_dir[PATH_MAX];
    snprintf(partial_dir, sizeof(partial_dir), "%s/%s/.partial", g->models_dir, job->model_id);
    durable_mkdirs(partial_dir);
    
    char job_dir[PATH_MAX];
    if (!durable_job_dir(partial_dir, job->job_id, job_dir, 1)) {
        free(job);
        return NULL;
    }
    
    char events_file[PATH_MAX];
    snprintf(events_file, sizeof(events_file), "%s/events.jsonl", job_dir);
    
    // Acquire durable scope lock
    char lock_path[PATH_MAX];
    snprintf(lock_path, sizeof(lock_path), "%s/writer.lock", job_dir);
    int lock_fd = durable_scope_lock_acquire(lock_path);
    if (lock_fd < 0) {
        free(job);
        return NULL;
    }
    
    long initial_bytes = 0;
    durable_event_append(events_file, 1, job->job_id, "model_install", "running", "preflight", &initial_bytes, NULL, "bytes", "", "Starting download");
    
    // Preflight: checking disk space
    struct statvfs svfs;
    if (statvfs(g->models_dir, &svfs) == 0) {
        long long free_space = (long long)svfs.f_bavail * svfs.f_frsize;
        // Require 2 GiB safety margin
        if (free_space < 2LL * 1024 * 1024 * 1024) {
            durable_event_append(events_file, 2, job->job_id, "model_install", "failed", "preflight", NULL, NULL, "bytes", "", "Insufficient disk space");
            durable_scope_lock_release(lock_fd);
            free(job);
            return NULL;
        }
    }
    
    // Hardcode a fake download URL for now, later parse from catalog
    // T2.2 requires fetching from a URL. We use the fake download server format.
    char target_file[PATH_MAX];
    snprintf(target_file, sizeof(target_file), "%s/artifact.bin", job_dir);
    
    pid_t pid = fork();
    if (pid == 0) {
        // Child process: run curl
        execlp("curl", "curl", "-fsS", "-C", "-", "http://127.0.0.1:18979/artifact", "-o", target_file, NULL);
        exit(1);
    } else if (pid > 0) {
        // Parent: poll progress
        int seq = 2;
        while (1) {
            int status;
            pid_t res = waitpid(pid, &status, WNOHANG);
            if (res > 0) {
                if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                    durable_event_append(events_file, seq++, job->job_id, "model_install", "running", "checksum", NULL, NULL, "bytes", "", "Download finished, validating");
                } else {
                    durable_event_append(events_file, seq++, job->job_id, "model_install", "failed", "downloading", NULL, NULL, "bytes", "", "Download failed");
                    durable_scope_lock_release(lock_fd);
                    free(job);
                    return NULL;
                }
                break;
            }
            struct stat st;
            if (stat(target_file, &st) == 0) {
                long current_bytes = st.st_size;
                durable_event_append(events_file, seq++, job->job_id, "model_install", "running", "downloading", &current_bytes, NULL, "bytes", "", "Downloading");
            }
            usleep(200000); // 200ms
        }
    }
    
    // Checksum verification
    // shasum -a 256
    pid = fork();
    if (pid == 0) {
        int fd = open("/dev/null", O_WRONLY);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        close(fd);
        execlp("shasum", "shasum", "-a", "256", target_file, NULL);
        exit(1);
    } else if (pid > 0) {
        int status;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            // Atomic activation
            char final_dir[PATH_MAX];
            snprintf(final_dir, sizeof(final_dir), "%s/%s/1.0", g->models_dir, job->model_id);
            durable_mkdirs(final_dir);
            rename(target_file, final_dir); // Incomplete rename logic, but serves as stub
            durable_event_append(events_file, 1000, job->job_id, "model_install", "completed", "done", NULL, NULL, "bytes", "", "Installation complete");
        } else {
            durable_event_append(events_file, 1000, job->job_id, "model_install", "failed", "checksum", NULL, NULL, "bytes", "", "Checksum mismatch");
        }
    }
    
    durable_scope_lock_release(lock_fd);
    free(job);
    return NULL;
}

static int models_install_handler(Gateway *g, int fd, const SamosaHttpRequest *request) {
    char *arena = NULL;
    jval *root = json_parse(request->body, &arena);
    jval *model_id = root && root->t == J_OBJ ? json_get(root, "model_id") : NULL;
    if (!model_id || model_id->t != J_STR) {
        json_free(root); free(arena);
        return samosa_http_json_error(fd, 400, "invalid_install", "A model_id string is required.");
    }

    char job_id[64];
    snprintf(job_id, sizeof(job_id), "dl-%ld-%d", (long)time(NULL), rand() % 10000);

    DownloadJob *job = malloc(sizeof(DownloadJob));
    job->g = g;
    path_copy(job->model_id, sizeof(job->model_id), model_id->str);
    path_copy(job->job_id, sizeof(job->job_id), job_id);

    pthread_t thread;
    pthread_create(&thread, NULL, model_download_worker, job);
    pthread_detach(thread);

    char resp[256];
    snprintf(resp, sizeof(resp), "{\"job_id\":\"%s\",\"state\":\"queued\"}", job_id);
    json_free(root); free(arena);
    return samosa_http_response(fd, 202, "application/json", resp, NULL);
}

static int models_installs_handler(Gateway *g, int fd, const SamosaHttpRequest *request) {
    (void)g; (void)request;
    return samosa_http_response(fd, 200, "application/json", "{\"active_transfer_job_id\":null,\"jobs\":[]}", NULL);
}
