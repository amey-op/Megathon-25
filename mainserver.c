/*
 The file for mainserver. Copy it entirely using CTRL + A
 */

#include <gio/gio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>
#include <errno.h>

#define DEFAULT_PORT 3000
#define MAX_CLIENTS 100
#define BUFFER_SIZE 8192
#define MAX_MESSAGE_SIZE 65536

typedef struct {
    GSocketConnection *connection;
    GOutputStream *output_stream;
    GInputStream *input_stream;
    GDataInputStream *data_stream;
    gchar *username;
    gint user_id;
    gint cursor_position;
    gboolean is_active;
    GMutex client_mutex;
} ClientInfo;

static GList *g_client_list = NULL;
static gint g_next_user_id = 1;
static GMutex g_clients_mutex;

static gchar *g_document_content = NULL;
static GMutex g_document_mutex;

// Store formatting information
typedef struct {
    gint start_pos;
    gint end_pos;
    gchar *tag_name;
    gchar *tag_value;
} FormatInfo;

static GList *g_format_list = NULL;
static GMutex g_format_mutex;

// Store media information
typedef struct {
    gchar *media_type;
    gchar *media_path;
} MediaInfo;

static GList *g_media_list = NULL;
static GMutex g_media_mutex;

typedef struct {
    gchar *content;
    gchar *timestamp;
    gchar *author;
    gint version_number;
} DocumentVersion;

static GList *g_version_history = NULL;
static gint g_current_version = 0;
static GMutex g_version_mutex;

// Forward declarations
static ClientInfo* find_client_by_connection(GSocketConnection *connection);
static void broadcast_to_all_except(ClientInfo *sender, const gchar *message);
static gboolean send_to_client(ClientInfo *client, const gchar *message);
static void apply_operation_to_document(const gchar *operation);
static void handle_login(ClientInfo *client, const gchar *username);
static void remove_client(ClientInfo *client);
static gboolean handle_client_message(ClientInfo *client, const gchar *message);
static void cleanup_client(ClientInfo *client);

static gchar* get_timestamp() {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    gchar *timestamp = g_strdup_printf("%04d-%02d-%02d %02d:%02d:%02d",
                                      t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
                                      t->tm_hour, t->tm_min, t->tm_sec);
    return timestamp;
}

static void log_message(const gchar *format, ...) {
    va_list args;
    va_start(args, format);
    
    gchar *timestamp = get_timestamp();
    printf("[%s] ", timestamp);
    vprintf(format, args);
    printf("\n");
    fflush(stdout);
    
    g_free(timestamp);
    va_end(args);
}

static ClientInfo* find_client_by_connection(GSocketConnection *connection) {
    g_mutex_lock(&g_clients_mutex);
    for (GList *l = g_client_list; l != NULL; l = l->next) {
        ClientInfo *client = (ClientInfo *)l->data;
        if (client->connection == connection) {
            g_mutex_unlock(&g_clients_mutex);
            return client;
        }
    }
    g_mutex_unlock(&g_clients_mutex);
    return NULL;
}

static gboolean send_to_client(ClientInfo *client, const gchar *message) {
    if (!client || !client->is_active) return FALSE;
    
    g_mutex_lock(&client->client_mutex);
    
    gchar *full_message = g_strdup_printf("%s\n", message);
    gsize bytes_written;
    GError *error = NULL;
    gboolean success = FALSE;
    
    success = g_output_stream_write_all(client->output_stream,
                                       full_message,
                                       strlen(full_message),
                                       &bytes_written,
                                       NULL,
                                       &error);
    
    if (!success || error) {
        if (error) {
            log_message("Error sending to user %s: %s", 
                       client->username ? client->username : "unknown", 
                       error->message);
            g_error_free(error);
        }
        client->is_active = FALSE;
    }
    
    g_free(full_message);
    g_mutex_unlock(&client->client_mutex);
    
    return success;
}

static void broadcast_to_all_except(ClientInfo *sender, const gchar *message) {
    g_mutex_lock(&g_clients_mutex);
    
    GList *clients_to_remove = NULL;
    
    for (GList *l = g_client_list; l != NULL; l = l->next) {
        ClientInfo *client = (ClientInfo *)l->data;
        
        if (client == sender || !client->is_active) {
            continue;
        }
        
        if (!send_to_client(client, message)) {
            clients_to_remove = g_list_append(clients_to_remove, client);
        }
    }
    
    for (GList *l = clients_to_remove; l != NULL; l = l->next) {
        ClientInfo *dead_client = (ClientInfo *)l->data;
        log_message("Removing dead client: %s", dead_client->username);
        dead_client->is_active = FALSE;
    }
    g_list_free(clients_to_remove);
    
    g_mutex_unlock(&g_clients_mutex);
}

static void broadcast_to_all(const gchar *message) {
    broadcast_to_all_except(NULL, message);
}

static void apply_operation_to_document(const gchar *operation) {
    g_mutex_lock(&g_document_mutex);
    
    gchar **parts = g_strsplit(operation, ":", 3);
    
    if (!parts || !parts[0]) {
        g_strfreev(parts);
        g_mutex_unlock(&g_document_mutex);
        return;
    }
    
    if (g_strcmp0(parts[0], "INSERT") == 0 && parts[1] && parts[2]) {
        gint pos = atoi(parts[1]);
        
        gint doc_len = g_document_content ? strlen(g_document_content) : 0;
        if (pos < 0) pos = 0;
        if (pos > doc_len) pos = doc_len;
        
        GString *unescaped = g_string_new("");
        const gchar *p = parts[2];
        while (*p) {
            if (*p == '\\' && *(p+1)) {
                p++;
                if (*p == 'n') {
                    g_string_append_c(unescaped, '\n');
                } else if (*p == 'r') {
                    g_string_append_c(unescaped, '\r');
                } else if (*p == 't') {
                    g_string_append_c(unescaped, '\t');
                } else if (*p == '\\') {
                    g_string_append_c(unescaped, '\\');
                } else if (*p == ':') {
                    g_string_append_c(unescaped, ':');
                } else {
                    g_string_append_c(unescaped, *p);
                }
                p++;
            } else {
                g_string_append_c(unescaped, *p);
                p++;
            }
        }
        
        if (!g_document_content) {
            g_document_content = g_string_free(unescaped, FALSE);
        } else {
            gchar *before = g_strndup(g_document_content, pos);
            gchar *after = g_strdup(g_document_content + pos);
            
            g_free(g_document_content);
            g_document_content = g_strconcat(before, unescaped->str, after, NULL);
            
            g_free(before);
            g_free(after);
            g_string_free(unescaped, TRUE);
        }
        
        log_message("Document updated: INSERT at position %d, new length: %zu", 
                   pos, strlen(g_document_content));
        
    } else if (g_strcmp0(parts[0], "DELETE") == 0 && parts[1] && parts[2]) {
        if (g_document_content) {
            gint start_pos = atoi(parts[1]);
            gint end_pos = atoi(parts[2]);
            
            gint doc_len = strlen(g_document_content);
            if (start_pos < 0) start_pos = 0;
            if (end_pos > doc_len) end_pos = doc_len;
            if (start_pos > end_pos) {
                gint temp = start_pos;
                start_pos = end_pos;
                end_pos = temp;
            }
            
            if (start_pos < end_pos) {
                gchar *before = g_strndup(g_document_content, start_pos);
                gchar *after = g_strdup(g_document_content + end_pos);
                
                g_free(g_document_content);
                g_document_content = g_strconcat(before, after, NULL);
                
                g_free(before);
                g_free(after);
                
                log_message("Document updated: DELETE from %d to %d, new length: %zu", 
                           start_pos, end_pos, strlen(g_document_content));
            }
        }
    }
    
    g_strfreev(parts);
    g_mutex_unlock(&g_document_mutex);
}

static void save_version(const gchar *author) {
    g_mutex_lock(&g_version_mutex);
    g_mutex_lock(&g_document_mutex);
    
    DocumentVersion *version = g_new(DocumentVersion, 1);
    version->content = g_strdup(g_document_content ? g_document_content : "");
    version->timestamp = get_timestamp();
    version->author = g_strdup(author);
    version->version_number = ++g_current_version;
    
    g_version_history = g_list_append(g_version_history, version);
    
    log_message("Version %d saved by %s", version->version_number, author);
    
    g_mutex_unlock(&g_document_mutex);
    g_mutex_unlock(&g_version_mutex);
}

static void send_all_formats_to_client(ClientInfo *client) {
    g_mutex_lock(&g_format_mutex);
    
    for (GList *l = g_format_list; l != NULL; l = l->next) {
        FormatInfo *fmt = (FormatInfo *)l->data;
        
        if (fmt->tag_value) {
            gchar *msg = g_strdup_printf("FORMAT:%s:%s:%d:%d",
                                        fmt->tag_name,
                                        fmt->tag_value,
                                        fmt->start_pos,
                                        fmt->end_pos);
            send_to_client(client, msg);
            g_free(msg);
        } else {
            gchar *msg = g_strdup_printf("FORMAT:%s:1:%d:%d",
                                        fmt->tag_name,
                                        fmt->start_pos,
                                        fmt->end_pos);
            send_to_client(client, msg);
            g_free(msg);
        }
    }
    
    g_mutex_unlock(&g_format_mutex);
}

static void send_all_media_to_client(ClientInfo *client) {
    g_mutex_lock(&g_media_mutex);
    
    for (GList *l = g_media_list; l != NULL; l = l->next) {
        MediaInfo *media = (MediaInfo *)l->data;
        
        gchar *msg = g_strdup_printf("MEDIA_INSERT:%s:%s",
                                    media->media_type,
                                    media->media_path);
        send_to_client(client, msg);
        g_free(msg);
    }
    
    g_mutex_unlock(&g_media_mutex);
}

static void handle_login(ClientInfo *client, const gchar *username) {
    if (!username || strlen(username) == 0) {
        log_message("Invalid login attempt: empty username");
        return;
    }
    
    g_free(client->username);
    client->username = g_strdup(username);
    
    log_message("User '%s' logged in (ID: %d)", username, client->user_id);
    
    gchar *response = g_strdup_printf("USER_ID:%d", client->user_id);
    if (!send_to_client(client, response)) {
        g_free(response);
        client->is_active = FALSE;
        return;
    }
    g_free(response);
    
    g_mutex_lock(&g_document_mutex);
    gchar *doc_msg = g_strdup_printf("INIT_DOCUMENT:%s", 
                                    g_document_content ? g_document_content : "");
    g_mutex_unlock(&g_document_mutex);
    
    if (!send_to_client(client, doc_msg)) {
        g_free(doc_msg);
        client->is_active = FALSE;
        return;
    }
    g_free(doc_msg);
    
    send_all_formats_to_client(client);
    send_all_media_to_client(client);
    
    g_mutex_lock(&g_clients_mutex);
    for (GList *l = g_client_list; l != NULL; l = l->next) {
        ClientInfo *other = (ClientInfo *)l->data;
        if (other->is_active && other != client && other->username) {
            gchar *user_msg = g_strdup_printf("USER_JOINED:%d:%s:%d",
                                             other->user_id,
                                             other->username,
                                             other->cursor_position);
            send_to_client(client, user_msg);
            g_free(user_msg);
        }
    }
    g_mutex_unlock(&g_clients_mutex);
    
    gchar *join_msg = g_strdup_printf("USER_JOINED:%d:%s:0", 
                                     client->user_id, username);
    broadcast_to_all_except(client, join_msg);
    g_free(join_msg);
}

static void handle_text_operation(ClientInfo *client, const gchar *operation) {
    if (!operation || strlen(operation) == 0) {
        return;
    }
    
    apply_operation_to_document(operation);
    
    gchar *msg = g_strdup_printf("TEXT_OP:%d:%s", client->user_id, operation);
    broadcast_to_all_except(client, msg);
    g_free(msg);
}

static void handle_cursor_move(ClientInfo *client, gint position) {
    client->cursor_position = position;
    
    gchar *msg = g_strdup_printf("CURSOR:%d:%d", client->user_id, position);
    broadcast_to_all_except(client, msg);
    g_free(msg);
}

static void handle_chat_message(ClientInfo *client, const gchar *message) {
    if (!message || strlen(message) == 0) {
        return;
    }
    
    log_message("Chat from %s: %s", 
               client->username ? client->username : "Anonymous", 
               message);
    
    gchar *chat_msg = g_strdup_printf("CHAT:%d:%s:%s", 
                                     client->user_id,
                                     client->username ? client->username : "Anonymous",
                                     message);
    broadcast_to_all(chat_msg);
    g_free(chat_msg);
}

static void handle_format_operation(ClientInfo *client, const gchar *format_data) {
    if (!format_data || strlen(format_data) == 0) {
        return;
    }
    
    gchar **parts = g_strsplit(format_data, ":", 4);
    
    if (!parts || !parts[0] || !parts[1] || !parts[2] || !parts[3]) {
        g_strfreev(parts);
        return;
    }
    
    gint start_pos = atoi(parts[2]);
    gint end_pos = atoi(parts[3]);
    
    if (start_pos < 0 || end_pos < 0 || start_pos > end_pos) {
        g_strfreev(parts);
        return;
    }
    
    g_mutex_lock(&g_format_mutex);
    
    if (g_strcmp0(parts[0], "color") == 0 || 
        g_strcmp0(parts[0], "bgcolor") == 0 ||
        g_strcmp0(parts[0], "size") == 0 ||
        g_strcmp0(parts[0], "align_left") == 0 ||
        g_strcmp0(parts[0], "align_center") == 0 ||
        g_strcmp0(parts[0], "align_right") == 0) {
        
        FormatInfo *fmt = g_new0(FormatInfo, 1);
        fmt->tag_name = g_strdup(parts[0]);
        fmt->tag_value = g_strdup(parts[1]);
        fmt->start_pos = start_pos;
        fmt->end_pos = end_pos;
        g_format_list = g_list_append(g_format_list, fmt);
        
    } else {
        gboolean apply = atoi(parts[1]) == 1;
        
        if (apply) {
            FormatInfo *fmt = g_new0(FormatInfo, 1);
            fmt->tag_name = g_strdup(parts[0]);
            fmt->tag_value = NULL;
            fmt->start_pos = start_pos;
            fmt->end_pos = end_pos;
            g_format_list = g_list_append(g_format_list, fmt);
        } else {
            for (GList *l = g_format_list; l != NULL; ) {
                FormatInfo *fmt = (FormatInfo *)l->data;
                GList *next = l->next;
                
                if (g_strcmp0(fmt->tag_name, parts[0]) == 0 &&
                    fmt->start_pos == start_pos &&
                    fmt->end_pos == end_pos) {
                    g_free(fmt->tag_name);
                    g_free(fmt->tag_value);
                    g_free(fmt);
                    g_format_list = g_list_delete_link(g_format_list, l);
                }
                l = next;
            }
        }
    }
    
    g_mutex_unlock(&g_format_mutex);
    
    gchar *msg = g_strdup_printf("FORMAT:%s", format_data);
    broadcast_to_all_except(client, msg);
    g_free(msg);
    
    g_strfreev(parts);
}

static void handle_terminal_output(ClientInfo *client, const gchar *output) {
    if (!output || strlen(output) == 0) {
        return;
    }
    
    log_message("Terminal output from %s", client->username);
    
    gchar *msg = g_strdup_printf("TERMINAL:%s", output);
    broadcast_to_all(msg);
    g_free(msg);
}

static void handle_media_insert(ClientInfo *client, const gchar *media_data) {
    if (!media_data || strlen(media_data) == 0) {
        return;
    }
    
    gchar **parts = g_strsplit(media_data, ":", 2);
    
    if (!parts || !parts[0] || !parts[1]) {
        g_strfreev(parts);
        return;
    }
    
    g_mutex_lock(&g_media_mutex);
    
    MediaInfo *media = g_new0(MediaInfo, 1);
    media->media_type = g_strdup(parts[0]);
    media->media_path = g_strdup(parts[1]);
    g_media_list = g_list_append(g_media_list, media);
    
    g_mutex_unlock(&g_media_mutex);
    
    log_message("Media inserted: %s (%s)", parts[1], parts[0]);
    
    gchar *msg = g_strdup_printf("MEDIA_INSERT:%s:%s", parts[0], parts[1]);
    broadcast_to_all_except(client, msg);
    g_free(msg);
    
    g_strfreev(parts);
}

static void handle_save_version(ClientInfo *client) {
    save_version(client->username ? client->username : "Anonymous");
    
    gchar *msg = g_strdup_printf("VERSION_SAVED:%d:%s", 
                                g_current_version,
                                client->username ? client->username : "Anonymous");
    broadcast_to_all(msg);
    g_free(msg);
}

static void handle_restore_version(ClientInfo *client, gint version_num) {
    g_mutex_lock(&g_version_mutex);
    
    DocumentVersion *target = NULL;
    for (GList *l = g_version_history; l != NULL; l = l->next) {
        DocumentVersion *v = (DocumentVersion *)l->data;
        if (v->version_number == version_num) {
            target = v;
            break;
        }
    }
    
    if (target) {
        g_mutex_lock(&g_document_mutex);
        g_free(g_document_content);
        g_document_content = g_strdup(target->content);
        g_mutex_unlock(&g_document_mutex);
        
        gchar *msg = g_strdup_printf("INIT_DOCUMENT:%s", g_document_content);
        broadcast_to_all(msg);
        g_free(msg);
        
        log_message("Version %d restored by %s", version_num, client->username);
    }
    
    g_mutex_unlock(&g_version_mutex);
}

static void cleanup_client(ClientInfo *client) {
    if (!client) return;
    
    log_message("Cleaning up client: %s (ID: %d)", 
               client->username ? client->username : "Unknown", 
               client->user_id);
    
    if (client->data_stream) {
        g_object_unref(client->data_stream);
        client->data_stream = NULL;
    }
    
    if (client->connection) {
        GError *error = NULL;
        g_io_stream_close(G_IO_STREAM(client->connection), NULL, &error);
        if (error) {
            log_message("Error closing connection: %s", error->message);
            g_error_free(error);
        }
        g_object_unref(client->connection);
        client->connection = NULL;
    }
    
    g_free(client->username);
    client->username = NULL;
}

static void remove_client(ClientInfo *client) {
    if (!client) return;
    
    log_message("User '%s' disconnected (ID: %d)", 
               client->username ? client->username : "Unknown", 
               client->user_id);
    
    if (client->is_active) {
        gchar *msg = g_strdup_printf("USER_LEFT:%d", client->user_id);
        broadcast_to_all_except(client, msg);
        g_free(msg);
    }
    
    g_mutex_lock(&g_clients_mutex);
    g_client_list = g_list_remove(g_client_list, client);
    g_mutex_unlock(&g_clients_mutex);
    
    cleanup_client(client);
    g_mutex_clear(&client->client_mutex);
    g_free(client);
}

static gboolean handle_client_message(ClientInfo *client, const gchar *message) {
    if (!message || strlen(message) == 0) {
        return TRUE;
    }
    
    if (strlen(message) > MAX_MESSAGE_SIZE) {
        log_message("Oversized message from %s, ignoring", client->username);
        return TRUE;
    }
    
    if (g_str_has_prefix(message, "LOGIN:")) {
        handle_login(client, message + 6);
    } else if (g_str_has_prefix(message, "INSERT:") || 
               g_str_has_prefix(message, "DELETE:")) {
        handle_text_operation(client, message);
    } else if (g_str_has_prefix(message, "CURSOR:")) {
        gint pos = atoi(message + 7);
        handle_cursor_move(client, pos);
    } else if (g_str_has_prefix(message, "FORMAT:")) {
        handle_format_operation(client, message + 7);
    } else if (g_str_has_prefix(message, "CHAT:")) {
        const gchar *chat_msg = message + 5;
        handle_chat_message(client, chat_msg);
    } else if (g_str_has_prefix(message, "TERMINAL:")) {
        const gchar *term_output = message + 9;
        handle_terminal_output(client, term_output);
    } else if (g_str_has_prefix(message, "MEDIA_INSERT:")) {
        const gchar *media_data = message + 13;
        handle_media_insert(client, media_data);
    } else if (g_str_has_prefix(message, "SAVE_VERSION")) {
        handle_save_version(client);
    } else if (g_str_has_prefix(message, "RESTORE_VERSION:")) {
        gint version = atoi(message + 16);
        handle_restore_version(client, version);
    } else if (g_str_has_prefix(message, "PING")) {
        send_to_client(client, "PONG");
    } else {
        log_message("Unknown message from %s: %s", 
                   client->username ? client->username : "Unknown",
                   message);
    }
    
    return TRUE;
}

static gpointer client_thread(gpointer data) {
    ClientInfo *client = (ClientInfo *)data;
    
    log_message("Client thread started for connection %d", client->user_id);
    
    client->data_stream = g_data_input_stream_new(client->input_stream);
    GError *error = NULL;
    
    while (client->is_active) {
        gchar *line = g_data_input_stream_read_line(client->data_stream, NULL, NULL, &error);
        
        if (error) {
            if (error->code != G_IO_ERROR_CLOSED) {
                log_message("Error reading from client %d: %s", client->user_id, error->message);
            }
            g_error_free(error);
            client->is_active = FALSE;
            break;
        }
        
        if (line == NULL) {
            log_message("Client %d disconnected (EOF)", client->user_id);
            client->is_active = FALSE;
            break;
        }
        
        g_strchug(line);
        g_strchomp(line);
        
        if (strlen(line) > 0) {
            handle_client_message(client, line);
        }
        
        g_free(line);
    }
    
    remove_client(client);
    log_message("Client thread finished for connection %d", client->user_id);
    
    return NULL;
}

static gboolean on_incoming_connection(GSocketService *service,
                                       GSocketConnection *connection,
                                       GObject *source_object,
                                       gpointer user_data) {
    log_message("New connection received");
    
    ClientInfo *client = g_new0(ClientInfo, 1);
    client->connection = g_object_ref(connection);
    client->output_stream = g_io_stream_get_output_stream(G_IO_STREAM(connection));
    client->input_stream = g_io_stream_get_input_stream(G_IO_STREAM(connection));
    client->username = NULL;
    client->user_id = g_next_user_id++;
    client->cursor_position = 0;
    client->is_active = TRUE;
    client->data_stream = NULL;
    g_mutex_init(&client->client_mutex);
    
    g_mutex_lock(&g_clients_mutex);
    g_client_list = g_list_append(g_client_list, client);
    gint client_count = g_list_length(g_client_list);
    g_mutex_unlock(&g_clients_mutex);
    
    log_message("Total clients connected: %d", client_count);
    
    GThread *thread = g_thread_new("client-thread", client_thread, client);
    if (thread) {
        g_thread_unref(thread);
    } else {
        log_message("Failed to create client thread");
        remove_client(client);
    }
    
    return TRUE;
}

int main(int argc, char *argv[]) {
    gint port = DEFAULT_PORT;
    
    if (argc > 1) {
        port = atoi(argv[1]);
        if (port <= 0 || port > 65535) {
            fprintf(stderr, "Invalid port number. Using default: %d\n", DEFAULT_PORT);
            port = DEFAULT_PORT;
        }
    }
    
    log_message("=== Collaborative Editor Server ===");
    log_message("Fixed version - Starting server on port %d", port);
    
    g_mutex_init(&g_clients_mutex);
    g_mutex_init(&g_document_mutex);
    g_mutex_init(&g_version_mutex);
    g_mutex_init(&g_format_mutex);
    g_mutex_init(&g_media_mutex);
    
    g_document_content = g_strdup("");
    
    GSocketService *service = g_socket_service_new();
    GError *error = NULL;
    
    GInetAddress *any_address = g_inet_address_new_any(G_SOCKET_FAMILY_IPV4);
    GSocketAddress *socket_address = g_inet_socket_address_new(any_address, port);
    g_object_unref(any_address);
    
    if (!g_socket_listener_add_address(G_SOCKET_LISTENER(service),
                                      socket_address,
                                      G_SOCKET_TYPE_STREAM,
                                      G_SOCKET_PROTOCOL_TCP,
                                      NULL,
                                      NULL,
                                      &error)) {
        fprintf(stderr, "Failed to start server: %s\n", error->message);
        g_error_free(error);
        g_object_unref(socket_address);
        return 1;
    }
    
    g_object_unref(socket_address);
    
    g_signal_connect(service, "incoming", G_CALLBACK(on_incoming_connection), NULL);
    g_socket_service_start(service);
    
    log_message("Server started successfully!");
    log_message("Waiting for connections...");
    log_message("Press Ctrl+C to stop the server");
    
    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(loop);
    
    log_message("Shutting down server...");
    
    g_mutex_lock(&g_clients_mutex);
    for (GList *l = g_client_list; l != NULL; l = l->next) {
        ClientInfo *client = (ClientInfo *)l->data;
        client->is_active = FALSE;
        cleanup_client(client);
    }
    g_list_free_full(g_client_list, g_free);
    g_client_list = NULL;
    g_mutex_unlock(&g_clients_mutex);
    
    g_socket_service_stop(service);
    g_object_unref(service);
    g_main_loop_unref(loop);
    
    g_free(g_document_content);
    g_document_content = NULL;
    
    g_list_free_full(g_format_list, (GDestroyNotify)g_free);
    g_format_list = NULL;
    
    g_list_free_full(g_media_list, (GDestroyNotify)g_free);
    g_media_list = NULL;
    
    g_list_free_full(g_version_history, (GDestroyNotify)g_free);
    g_version_history = NULL;
    
    g_mutex_clear(&g_clients_mutex);
    g_mutex_clear(&g_document_mutex);
    g_mutex_clear(&g_version_mutex);
    g_mutex_clear(&g_format_mutex);
    g_mutex_clear(&g_media_mutex);
    
    log_message("Server stopped cleanly");
    return 0;
}
