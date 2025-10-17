/*
    ADD YOUR WIFI IP ADDRESS AT LINE NO 61 OF THE CODE
*/

#include <gtk/gtk.h>
#include <gio/gio.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <pango/pangocairo.h>
#include <curl/curl.h>
#include <json-glib/json-glib.h>

// --- App State Management ---
GtkApplication *g_app = NULL;
GtkWidget *g_main_window = NULL;
GtkWidget *g_stack = NULL;
gchar *g_logged_in_user = NULL;

// --- UI Globals ---
GtkWidget *g_textview_editor = NULL;
GtkWidget *g_textview_terminal = NULL;
GtkWidget *g_sidebar_notebook = NULL;
gboolean g_sidebar_is_visible = FALSE;
char *g_current_filepath = NULL;
GtkWidget *g_media_gallery_view = NULL;
static GList *media_gallery_list = NULL;

// --- Autocomplete Globals ---
GtkWidget *g_completion_popup = NULL;
GtkWidget *g_completion_listbox = NULL;
gboolean g_is_autocompleting = FALSE;

// --- Formatting Globals ---
GtkTextTag *g_tag_bold = NULL;
GtkTextTag *g_tag_italic = NULL;
GtkTextTag *g_tag_underline = NULL;
GtkTextTag *g_tag_align_left = NULL;
GtkTextTag *g_tag_align_center = NULL;
GtkTextTag *g_tag_align_right = NULL;
gint g_current_font_size = 12;

// --- Interactive Execution Globals ---
GPid g_child_pid = 0;
GIOChannel *g_stdin_channel = NULL;
GIOChannel *g_stdout_channel = NULL;
GIOChannel *g_stderr_channel = NULL;
GtkWidget *g_terminal_input_entry = NULL;
guint g_stdout_watch_id = 0;
guint g_stderr_watch_id = 0;

// --- Networking Globals ---
#define DEFAULT_PORT 3000
GSocketConnection *g_server_connection = NULL;
gboolean g_is_updating_from_network = FALSE;
GDataInputStream *g_server_input_stream = NULL;

const char *g_server_host = "10.151.48.149"; // ADD YOUR IP COMMON WIFI IP ADDRESS HERE

// --- Remote User Tracking ---
typedef struct {
    gint user_id;
    gchar *username;
    gint cursor_position;
    GtkTextMark *cursor_mark;
    GtkTextTag *cursor_tag;
} RemoteUser;

static GList *g_remote_users = NULL;
static gint g_my_user_id = -1;
static guint g_cursor_update_timer = 0;

// --- Chat Globals ---
static GtkWidget *g_chat_textview = NULL;
static GtkWidget *g_chat_entry = NULL;

// --- AI Chat Globals ---
static GtkWidget *g_ai_chat_textview = NULL;
static GtkWidget *g_ai_chat_entry = NULL;
static GtkWidget *g_ai_chat_send_button = NULL;
#define GEMINI_API_KEY "AIzaSyCca07ou42o7HqDwSdBrPq4wmSdmJ0i_yg"
#define GEMINI_API_URL "https://generativelanguage.googleapis.com/v1beta/models/gemini-pro:generateContent?key=" GEMINI_API_KEY

// --- Version History Structures ---
typedef struct {
    gchar *content;
    gchar *timestamp;
    gint version_number;
} EditorVersion;

static GList *g_version_history = NULL;
static gint g_current_version_number = 0;
static gchar *g_version_history_file = NULL;

// --- Forward Declarations ---
static void send_operation_to_server(const gchar *operation);
static void send_format_to_server(const gchar *tag_name, const gchar *value, gint start, gint end);
static void append_terminal_output(const gchar *text);
static void queue_cursor_update();
static gboolean check_user_credentials(const gchar *username, const gchar *password);
static gboolean register_user(const gchar *username, const gchar *password);
static void start_reading_from_server();
static void append_ai_chat_message(const gchar *message, gboolean is_user);
static void send_ai_chat_message(const gchar *message);

// --- User Authentication Functions ---

static gboolean check_user_credentials(const gchar *username, const gchar *password) {
    FILE *file = fopen("users.txt", "r");
    if (!file) return FALSE;
    char line[256];
    gboolean found = FALSE;
    gchar *expected_line_content = g_strdup_printf("%s:%s", username, password);

    while (fgets(line, sizeof(line), file)) {
        line[strcspn(line, "\n\r")] = '\0';
        if (strcmp(line, expected_line_content) == 0) {
            found = TRUE;
            break;
        }
    }
    g_free(expected_line_content);
    fclose(file);
    return found;
}

static gboolean register_user(const gchar *username, const gchar *password) {
    FILE *file = fopen("users.txt", "r");
    if (file) {
        char line[256];
        while (fgets(line, sizeof(line), file)) {
            line[strcspn(line, "\n\r")] = '\0';
            gchar *stored_user = g_strndup(line, strcspn(line, ":"));
            if (g_strcmp0(stored_user, username) == 0) {
                g_free(stored_user);
                fclose(file);
                return FALSE;
            }
            g_free(stored_user);
        }
        fclose(file);
    }
    
    file = fopen("users.txt", "a");
    if (!file) return FALSE;
    fprintf(file, "%s:%s\n", username, password);
    fclose(file);
    return TRUE;
}

// --- Utility & Terminal Functions ---

static void append_terminal_output(const gchar *text) {
    if (!g_textview_terminal) return;
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(g_textview_terminal));
    GtkTextIter end_iter;
    gtk_text_buffer_get_end_iter(buffer, &end_iter);
    gtk_text_buffer_insert(buffer, &end_iter, text, -1);
    
    GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(gtk_widget_get_parent(g_textview_terminal)));
    gtk_adjustment_set_value(adj, gtk_adjustment_get_upper(adj));
}

static void set_terminal_output(const gchar *text) {
    if (!g_textview_terminal) return;
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(g_textview_terminal));
    gtk_text_buffer_set_text(buffer, text, -1);
}

static gboolean save_content_to_file(const gchar *filename, GtkWidget *textview) {
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(textview));
    GtkTextIter start, end;
    gchar *text;
    gtk_text_buffer_get_start_iter(buffer, &start);
    gtk_text_buffer_get_end_iter(buffer, &end);
    text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
    gboolean success = g_file_set_contents(filename, text, -1, NULL);
    g_free(text);
    return success;
}

// --- Remote User Management ---

static RemoteUser* find_remote_user(gint user_id) {
    for (GList *l = g_remote_users; l != NULL; l = l->next) {
        RemoteUser *user = (RemoteUser *)l->data;
        if (user->user_id == user_id) return user;
    }
    return NULL;
}

static void add_remote_user(gint user_id, const gchar *username, gint cursor_pos) {
    RemoteUser *user = g_new0(RemoteUser, 1);
    user->user_id = user_id;
    user->username = g_strdup(username);
    user->cursor_position = cursor_pos;
    
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(g_textview_editor));
    GtkTextIter iter;
    gtk_text_buffer_get_iter_at_offset(buffer, &iter, cursor_pos);
    
    gchar *mark_name = g_strdup_printf("user_%d_cursor", user_id);
    user->cursor_mark = gtk_text_buffer_create_mark(buffer, mark_name, &iter, TRUE);
    g_free(mark_name);
    
    GdkRGBA color;
    color.red = (gdouble)((user_id * 37) % 100) / 100.0;
    color.green = (gdouble)((user_id * 73) % 100) / 100.0;
    color.blue = (gdouble)((user_id * 127) % 100) / 100.0;
    color.alpha = 0.3;
    
    gchar *tag_name = g_strdup_printf("cursor_%d", user_id);
    user->cursor_tag = gtk_text_buffer_create_tag(buffer, tag_name, 
                                                   "background-rgba", &color, 
                                                   NULL);
    g_free(tag_name);
    
    g_remote_users = g_list_append(g_remote_users, user);
}

static void remove_remote_user(gint user_id) {
    RemoteUser *user = find_remote_user(user_id);
    if (user) {
        GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(g_textview_editor));
        if (user->cursor_mark) gtk_text_buffer_delete_mark(buffer, user->cursor_mark);
        g_free(user->username);
        g_remote_users = g_list_remove(g_remote_users, user);
        g_free(user);
    }
}

static void update_remote_cursor(gint user_id, gint position) {
    RemoteUser *user = find_remote_user(user_id);
    if (!user) return;
    
    user->cursor_position = position;
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(g_textview_editor));
    GtkTextIter iter, iter_end;
    
    gtk_text_buffer_get_start_iter(buffer, &iter);
    gtk_text_buffer_get_end_iter(buffer, &iter_end);
    gtk_text_buffer_remove_tag(buffer, user->cursor_tag, &iter, &iter_end);
    
    gtk_text_buffer_get_iter_at_offset(buffer, &iter, position);
    gtk_text_buffer_move_mark(buffer, user->cursor_mark, &iter);
    
    iter_end = iter;
    if (gtk_text_iter_forward_char(&iter_end)) {
        gtk_text_buffer_apply_tag(buffer, user->cursor_tag, &iter, &iter_end);
    }
}

static gboolean on_cursor_position_changed(gpointer user_data) {
    if (g_is_updating_from_network || !g_server_connection) {
        g_cursor_update_timer = 0;
        return FALSE;
    }
    
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(g_textview_editor));
    GtkTextMark *insert_mark = gtk_text_buffer_get_insert(buffer);
    GtkTextIter iter;
    gtk_text_buffer_get_iter_at_mark(buffer, &iter, insert_mark);
    gint position = gtk_text_iter_get_offset(&iter);
    
    gchar *msg = g_strdup_printf("CURSOR:%d\n", position);
    GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(g_server_connection));
    g_output_stream_write_async(out, msg, strlen(msg), G_PRIORITY_DEFAULT, NULL, NULL, NULL);
    g_free(msg);
    
    g_cursor_update_timer = 0;
    return FALSE;
}

static void queue_cursor_update() {
    if (g_cursor_update_timer > 0) {
        g_source_remove(g_cursor_update_timer);
    }
    g_cursor_update_timer = g_timeout_add(50, on_cursor_position_changed, NULL);
}

// --- Chat Functions ---

static void display_chat_message(const gchar *username, const gchar *message) {
    if (!g_chat_textview) {
        append_terminal_output("ERROR: Chat textview is NULL\n");
        return;
    }
    
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(g_chat_textview));
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(buffer, &end);
    
    GDateTime *now = g_date_time_new_now_local();
    gchar *timestamp = g_date_time_format(now, "%H:%M");
    g_date_time_unref(now);
    
    gchar *formatted = g_strdup_printf("[%s] %s: %s\n", timestamp, username, message);
    gtk_text_buffer_insert(buffer, &end, formatted, -1);
    
    gtk_text_buffer_get_end_iter(buffer, &end);
    GtkTextMark *end_mark = gtk_text_buffer_create_mark(buffer, NULL, &end, FALSE);
    gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(g_chat_textview), end_mark, 0.0, TRUE, 0.0, 1.0);
    gtk_text_buffer_delete_mark(buffer, end_mark);
    
    g_free(timestamp);
    g_free(formatted);
}

static void send_chat_message(const gchar *message) {
    if (!g_server_connection || !message || strlen(message) == 0) return;
    
    GString *escaped = g_string_new("");
    for (const gchar *p = message; *p; p++) {
        if (*p == '\n') {
            g_string_append(escaped, "\\n");
        } else if (*p == ':') {
            g_string_append(escaped, "\\:");
        } else if (*p == '\\') {
            g_string_append(escaped, "\\\\");
        } else {
            g_string_append_c(escaped, *p);
        }
    }
    
    gchar *msg = g_strdup_printf("CHAT:%s\n", escaped->str);
    GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(g_server_connection));
    gsize bytes_written;
    g_output_stream_write_all(out, msg, strlen(msg), &bytes_written, NULL, NULL);
    g_free(msg);
    g_string_free(escaped, TRUE);
}

static void on_chat_send_clicked(GtkWidget *widget, gpointer data) {
    if (!g_chat_entry) return;
    
    const gchar *text = gtk_entry_get_text(GTK_ENTRY(g_chat_entry));
    if (text && strlen(text) > 0) {
        send_chat_message(text);
        gtk_entry_set_text(GTK_ENTRY(g_chat_entry), "");
    }
}

// --- AI Chat Functions ---

typedef struct {
    gchar *response_text;
    GError *error;
} CurlResponse;

// Original (Error-causing)
// static size_t curl_write_callback(void *contents, size_t size, size_t nmemb, void *userp) {

// Corrected
static size_t ai_chat_curl_write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    CurlResponse *response = (CurlResponse *)userp;

    if (response->response_text == NULL) {
        response->response_text = g_malloc(realsize + 1);
        memcpy(response->response_text, contents, realsize);
        response->response_text[realsize] = '\0';
    } else {
        gchar *temp = g_realloc(response->response_text, strlen(response->response_text) + realsize + 1);
        response->response_text = temp;
        memcpy(response->response_text + strlen(response->response_text), contents, realsize);
        response->response_text[strlen(response->response_text) + realsize] = '\0';
    }

    return realsize;
}

static void append_ai_chat_message(const gchar *message, gboolean is_user) {
    if (!g_ai_chat_textview) return;
    
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(g_ai_chat_textview));
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(buffer, &end);
    
    GDateTime *now = g_date_time_new_now_local();
    gchar *timestamp = g_date_time_format(now, "%H:%M");
    g_date_time_unref(now);
    
    const gchar *sender = is_user ? "You" : "Gemini";
    gchar *formatted = g_strdup_printf("[%s] %s: %s\n\n", timestamp, sender, message);
    gtk_text_buffer_insert(buffer, &end, formatted, -1);
    
    gtk_text_buffer_get_end_iter(buffer, &end);
    GtkTextMark *end_mark = gtk_text_buffer_create_mark(buffer, NULL, &end, FALSE);
    gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(g_ai_chat_textview), end_mark, 0.0, TRUE, 0.0, 1.0);
    gtk_text_buffer_delete_mark(buffer, end_mark);
    
    g_free(timestamp);
    g_free(formatted);
}

static gpointer send_ai_chat_request_thread(gpointer data) {
    gchar *user_message = (gchar *)data;
    
    CURL *curl = curl_easy_init();
    if (!curl) {
        g_idle_add((GSourceFunc)gtk_widget_set_sensitive, g_ai_chat_send_button);
        g_free(user_message);
        return NULL;
    }
    
    JsonBuilder *builder = json_builder_new();
    json_builder_begin_object(builder);
    
    json_builder_set_member_name(builder, "contents");
    json_builder_begin_array(builder);
    json_builder_begin_object(builder);
    
    json_builder_set_member_name(builder, "parts");
    json_builder_begin_array(builder);
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "text");
    json_builder_add_string_value(builder, user_message);
    json_builder_end_object(builder);
    json_builder_end_array(builder);
    
    json_builder_end_object(builder);
    json_builder_end_array(builder);
    json_builder_end_object(builder);
    
    JsonGenerator *generator = json_generator_new();
    JsonNode *root = json_builder_get_root(builder);
    json_generator_set_root(generator, root);
    gchar *json_data = json_generator_to_data(generator, NULL);
    
    CurlResponse response = {NULL, NULL};
    
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    
   // Original (Error-causing)
// curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_callback);
// curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

// Corrected
    curl_easy_setopt(curl, CURLOPT_URL, GEMINI_API_URL);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_data);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, ai_chat_curl_write_callback); // <-- RENAME HERE
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    
    CURLcode res = curl_easy_perform(curl);
    
    if (res == CURLE_OK && response.response_text) {
        JsonParser *parser = json_parser_new();
        if (json_parser_load_from_data(parser, response.response_text, -1, NULL)) {
            JsonNode *root_node = json_parser_get_root(parser);
            JsonObject *root_obj = json_node_get_object(root_node);
            
            if (json_object_has_member(root_obj, "candidates")) {
                JsonArray *candidates = json_object_get_array_member(root_obj, "candidates");
                if (json_array_get_length(candidates) > 0) {
                    JsonObject *candidate = json_array_get_object_element(candidates, 0);
                    JsonObject *content = json_object_get_object_member(candidate, "content");
                    JsonArray *parts = json_object_get_array_member(content, "parts");
                    if (json_array_get_length(parts) > 0) {
                        JsonObject *part = json_array_get_object_element(parts, 0);
                        const gchar *text = json_object_get_string_member(part, "text");
                        
                        gchar *text_copy = g_strdup(text);
                        g_idle_add((GSourceFunc)append_ai_chat_message, text_copy);
                        g_idle_add((GSourceFunc)g_free, text_copy);
                    }
                }
            } else {
                g_idle_add((GSourceFunc)append_ai_chat_message, g_strdup("Error: Unable to get response from Gemini"));
            }
        }
        g_object_unref(parser);
    } else {
        gchar *error_msg = g_strdup_printf("Error: %s", curl_easy_strerror(res));
        g_idle_add((GSourceFunc)append_ai_chat_message, error_msg);
        g_idle_add((GSourceFunc)g_free, error_msg);
    }
    
    if (response.response_text) g_free(response.response_text);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    g_free(json_data);
    json_node_free(root);
    g_object_unref(generator);
    g_object_unref(builder);
    g_free(user_message);
    
    g_idle_add((GSourceFunc)gtk_widget_set_sensitive, g_ai_chat_send_button);
    
    return NULL;
}

static void send_ai_chat_message(const gchar *message) {
    if (!message || strlen(message) == 0) return;
    
    append_ai_chat_message(message, TRUE);
    
    gtk_widget_set_sensitive(g_ai_chat_send_button, FALSE);
    
    gchar *message_copy = g_strdup(message);
    g_thread_new("ai_chat_thread", send_ai_chat_request_thread, message_copy);
}

static void on_ai_chat_send_clicked(GtkWidget *widget, gpointer data) {
    if (!g_ai_chat_entry) return;
    
    const gchar *text = gtk_entry_get_text(GTK_ENTRY(g_ai_chat_entry));
    if (text && strlen(text) > 0) {
        send_ai_chat_message(text);
        gtk_entry_set_text(GTK_ENTRY(g_ai_chat_entry), "");
    }
}

// --- Version History Implementation ---

static void save_current_version() {
    if (!g_textview_editor) return;
    
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(g_textview_editor));
    GtkTextIter start, end;
    gtk_text_buffer_get_start_iter(buffer, &start);
    gtk_text_buffer_get_end_iter(buffer, &end);
    gchar *content = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
    
    EditorVersion *version = g_new(EditorVersion, 1);
    version->content = content;
    version->version_number = ++g_current_version_number;
    
    GDateTime *now = g_date_time_new_now_local();
    version->timestamp = g_date_time_format(now, "%Y-%m-%d %H:%M:%S");
    g_date_time_unref(now);
    
    g_version_history = g_list_append(g_version_history, version);
    
    gchar *msg = g_strdup_printf("Version %d saved at %s\n", version->version_number, version->timestamp);
    append_terminal_output(msg);
    g_free(msg);
    
    send_operation_to_server("SAVE_VERSION");
}

static void restore_version(gint version_number) {
    EditorVersion *target_version = NULL;
    
    for (GList *l = g_version_history; l != NULL; l = l->next) {
        EditorVersion *v = (EditorVersion *)l->data;
        if (v->version_number == version_number) {
            target_version = v;
            break;
        }
    }
    
    if (!target_version) {
        append_terminal_output("Error: Version not found.\n");
        return;
    }
    
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(g_textview_editor));
    g_is_updating_from_network = TRUE;
    gtk_text_buffer_set_text(buffer, target_version->content, -1);
    g_is_updating_from_network = FALSE;
    
    gchar *msg = g_strdup_printf("Restored to version %d (%s)\n", target_version->version_number, target_version->timestamp);
    append_terminal_output(msg);
    g_free(msg);
}

static void on_version_row_activated(GtkTreeView *tree_view, GtkTreePath *path, GtkTreeViewColumn *column, gpointer user_data) {
    GtkTreeModel *model = gtk_tree_view_get_model(tree_view);
    GtkTreeIter iter;
    
    if (gtk_tree_model_get_iter(model, &iter, path)) {
        gint version_num;
        gtk_tree_model_get(model, &iter, 0, &version_num, -1);
        restore_version(version_num);
        
        GtkWidget *dialog = GTK_WIDGET(user_data);
        gtk_widget_destroy(dialog);
    }
}

static void show_version_history_dialog(GtkWidget *widget, gpointer data) {
    GtkWidget *dialog = gtk_dialog_new_with_buttons("Version History",
                                                    GTK_WINDOW(g_main_window),
                                                    GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                                    "_Close", GTK_RESPONSE_CLOSE,
                                                    NULL);
    
    gtk_window_set_default_size(GTK_WINDOW(dialog), 600, 400);
    
    GtkWidget *content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    
    GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_widget_set_vexpand(scrolled, TRUE);
    gtk_box_pack_start(GTK_BOX(content_area), scrolled, TRUE, TRUE, 5);
    
    GtkListStore *store = gtk_list_store_new(3, G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING);
    
    for (GList *l = g_list_last(g_version_history); l != NULL; l = l->prev) {
        EditorVersion *v = (EditorVersion *)l->data;
        GtkTreeIter iter;
        gtk_list_store_append(store, &iter);
        
        gchar *preview = g_strndup(v->content, 100);
        gchar *preview_clean = g_strdup(preview);
        for (gchar *p = preview_clean; *p; p++) {
            if (*p == '\n' || *p == '\r') *p = ' ';
        }
        
        gtk_list_store_set(store, &iter, 0, v->version_number, 1, v->timestamp, 2, preview_clean, -1);
        
        g_free(preview);
        g_free(preview_clean);
    }
    
    GtkWidget *tree_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    g_object_unref(store);
    
    GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *col;
    
    col = gtk_tree_view_column_new_with_attributes("Version", renderer, "text", 0, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree_view), col);
    
    col = gtk_tree_view_column_new_with_attributes("Timestamp", renderer, "text", 1, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree_view), col);
    
    col = gtk_tree_view_column_new_with_attributes("Preview", renderer, "text", 2, NULL);
    gtk_tree_view_column_set_expand(col, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree_view), col);
    
    g_signal_connect(tree_view, "row-activated", G_CALLBACK(on_version_row_activated), dialog);
    
    gtk_container_add(GTK_CONTAINER(scrolled), tree_view);
    
    GtkWidget *label = gtk_label_new("Double-click a version to restore it");
    gtk_box_pack_start(GTK_BOX(content_area), label, FALSE, FALSE, 5);
    
    gtk_widget_show_all(dialog);
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

static void on_save_clicked(GtkWidget *widget, gpointer data) {
    save_current_version();
}

static void on_save_as_clicked(GtkWidget *widget, gpointer data) {
    GtkWidget *dialog;
    dialog = gtk_file_chooser_dialog_new("Save File As", GTK_WINDOW(g_main_window),
                                         GTK_FILE_CHOOSER_ACTION_SAVE,
                                         "_Cancel", GTK_RESPONSE_CANCEL,
                                         "_Save", GTK_RESPONSE_ACCEPT, NULL);
    
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dialog), TRUE);
    
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        if (save_content_to_file(filename, g_textview_editor)) {
            gchar *msg = g_strdup_printf("File saved to: %s\n", filename);
            append_terminal_output(msg);
            g_free(msg);
        } else {
            append_terminal_output("Error: Could not save file.\n");
        }
        g_free(filename);
    }
    gtk_widget_destroy(dialog);
}

static void on_undo_version_clicked(GtkWidget *widget, gpointer data) {
    if (g_version_history == NULL || g_list_length(g_version_history) < 2) {
        append_terminal_output("No previous version available.\n");
        return;
    }
    
    GList *second_last = g_list_nth(g_version_history, g_list_length(g_version_history) - 2);
    if (second_last) {
        EditorVersion *v = (EditorVersion *)second_last->data;
        restore_version(v->version_number);
    }
}

// --- Formatting Functions ---

static void send_format_to_server(const gchar *tag_name, const gchar *value, gint start, gint end) {
    if (!g_server_connection || g_is_updating_from_network) return;
    
    gchar *format_msg = g_strdup_printf("FORMAT:%s:%s:%d:%d\n", 
                                       tag_name, value, start, end);
    GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(g_server_connection));
    g_output_stream_write_async(out, format_msg, strlen(format_msg), G_PRIORITY_DEFAULT, NULL, NULL, NULL);
    g_free(format_msg);
}

static void apply_tag_to_selection(GtkTextTag *tag, gboolean apply, const gchar *tag_name) {
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(g_textview_editor));
    GtkTextIter start, end;
    
    if (gtk_text_buffer_get_selection_bounds(buffer, &start, &end)) {
        if (apply) {
            gtk_text_buffer_apply_tag(buffer, tag, &start, &end);
        } else {
            gtk_text_buffer_remove_tag(buffer, tag, &start, &end);
        }
        
        gint start_pos = gtk_text_iter_get_offset(&start);
        gint end_pos = gtk_text_iter_get_offset(&end);
        
        gchar *value = apply ? "1" : "0";
        send_format_to_server(tag_name, value, start_pos, end_pos);
    }
}

static void on_bold_clicked(GtkWidget *widget, gpointer data) {
    GtkToggleButton *toggle = GTK_TOGGLE_BUTTON(widget);
    gboolean is_active = gtk_toggle_button_get_active(toggle);
    apply_tag_to_selection(g_tag_bold, is_active, "bold");
}

static void on_italic_clicked(GtkWidget *widget, gpointer data) {
    GtkToggleButton *toggle = GTK_TOGGLE_BUTTON(widget);
    gboolean is_active = gtk_toggle_button_get_active(toggle);
    apply_tag_to_selection(g_tag_italic, is_active, "italic");
}

static void on_underline_clicked(GtkWidget *widget, gpointer data) {
    GtkToggleButton *toggle = GTK_TOGGLE_BUTTON(widget);
    gboolean is_active = gtk_toggle_button_get_active(toggle);
    apply_tag_to_selection(g_tag_underline, is_active, "underline");
}

static void on_align_left_clicked(GtkWidget *widget, gpointer data) {
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(g_textview_editor));
    GtkTextIter start, end;
    
    if (gtk_text_buffer_get_selection_bounds(buffer, &start, &end)) {
        gtk_text_buffer_remove_tag(buffer, g_tag_align_center, &start, &end);
        gtk_text_buffer_remove_tag(buffer, g_tag_align_right, &start, &end);
        gtk_text_buffer_apply_tag(buffer, g_tag_align_left, &start, &end);
        
        gint start_pos = gtk_text_iter_get_offset(&start);
        gint end_pos = gtk_text_iter_get_offset(&end);
        send_format_to_server("align_left", "1", start_pos, end_pos);
    }
}

static void on_align_center_clicked(GtkWidget *widget, gpointer data) {
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(g_textview_editor));
    GtkTextIter start, end;
    
    if (gtk_text_buffer_get_selection_bounds(buffer, &start, &end)) {
        gtk_text_buffer_remove_tag(buffer, g_tag_align_left, &start, &end);
        gtk_text_buffer_remove_tag(buffer, g_tag_align_right, &start, &end);
        gtk_text_buffer_apply_tag(buffer, g_tag_align_center, &start, &end);
        
        gint start_pos = gtk_text_iter_get_offset(&start);
        gint end_pos = gtk_text_iter_get_offset(&end);
        send_format_to_server("align_center", "1", start_pos, end_pos);
    }
}

static void on_align_right_clicked(GtkWidget *widget, gpointer data) {
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(g_textview_editor));
    GtkTextIter start, end;
    
    if (gtk_text_buffer_get_selection_bounds(buffer, &start, &end)) {
        gtk_text_buffer_remove_tag(buffer, g_tag_align_left, &start, &end);
        gtk_text_buffer_remove_tag(buffer, g_tag_align_center, &start, &end);
        gtk_text_buffer_apply_tag(buffer, g_tag_align_right, &start, &end);
        
        gint start_pos = gtk_text_iter_get_offset(&start);
        gint end_pos = gtk_text_iter_get_offset(&end);
        send_format_to_server("align_right", "1", start_pos, end_pos);
    }
}

static void on_font_size_changed(GtkWidget *widget, gpointer data) {
    const gchar *text = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(widget));
    if (text) {
        gint size = atoi(text);
        if (size > 0) {
            g_current_font_size = size;
            GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(g_textview_editor));
            GtkTextIter start, end;
            if (gtk_text_buffer_get_selection_bounds(buffer, &start, &end)) {
                gchar *tag_name = g_strdup_printf("size_%d", size);
                GtkTextTag *size_tag = gtk_text_buffer_create_tag(buffer, tag_name, 
                                                                  "size-points", (gdouble)size, NULL);
                gtk_text_buffer_apply_tag(buffer, size_tag, &start, &end);
                
                gint start_pos = gtk_text_iter_get_offset(&start);
                gint end_pos = gtk_text_iter_get_offset(&end);
                
                gchar *size_str = g_strdup_printf("%d", size);
                send_format_to_server("size", size_str, start_pos, end_pos);
                g_free(size_str);
                g_free(tag_name);
            }
        }
    }
}

static void on_text_color_clicked(GtkWidget *widget, gpointer data) {
    GtkWidget *dialog = gtk_color_chooser_dialog_new("Choose Text Color", GTK_WINDOW(g_main_window));
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK) {
        GdkRGBA color;
        gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(dialog), &color);
        GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(g_textview_editor));
        GtkTextIter start, end;
        if (gtk_text_buffer_get_selection_bounds(buffer, &start, &end)) {
            gchar *tag_name = g_strdup_printf("color_%d_%d_%d", 
                                             (int)(color.red * 255),
                                             (int)(color.green * 255),
                                             (int)(color.blue * 255));
            GtkTextTag *color_tag = gtk_text_buffer_create_tag(buffer, tag_name, 
                                                               "foreground-rgba", &color, NULL);
            gtk_text_buffer_apply_tag(buffer, color_tag, &start, &end);
            
            gint start_pos = gtk_text_iter_get_offset(&start);
            gint end_pos = gtk_text_iter_get_offset(&end);
            
            gchar *color_str = g_strdup_printf("#%02x%02x%02x",
                                              (int)(color.red * 255),
                                              (int)(color.green * 255),
                                              (int)(color.blue * 255));
            send_format_to_server("color", color_str, start_pos, end_pos);
            g_free(color_str);
            g_free(tag_name);
        }
    }
    gtk_widget_destroy(dialog);
}

static void on_bg_color_clicked(GtkWidget *widget, gpointer data) {
    GtkWidget *dialog = gtk_color_chooser_dialog_new("Choose Background Color", GTK_WINDOW(g_main_window));
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK) {
        GdkRGBA color;
        gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(dialog), &color);
        GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(g_textview_editor));
        GtkTextIter start, end;
        if (gtk_text_buffer_get_selection_bounds(buffer, &start, &end)) {
            gchar *tag_name = g_strdup_printf("bgcolor_%d_%d_%d",
                                             (int)(color.red * 255),
                                             (int)(color.green * 255),
                                             (int)(color.blue * 255));
            GtkTextTag *bg_tag = gtk_text_buffer_create_tag(buffer, tag_name, 
                                                            "background-rgba", &color, NULL);
            gtk_text_buffer_apply_tag(buffer, bg_tag, &start, &end);
            
            gint start_pos = gtk_text_iter_get_offset(&start);
            gint end_pos = gtk_text_iter_get_offset(&end);
            
            gchar *color_str = g_strdup_printf("#%02x%02x%02x",
                                              (int)(color.red * 255),
                                              (int)(color.green * 255),
                                              (int)(color.blue * 255));
            send_format_to_server("bgcolor", color_str, start_pos, end_pos);
            g_free(color_str);
            g_free(tag_name);
        }
    }
    gtk_widget_destroy(dialog);
}

static void on_clear_formatting_clicked(GtkWidget *widget, gpointer data) {
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(g_textview_editor));
    GtkTextIter start, end;
    if (gtk_text_buffer_get_selection_bounds(buffer, &start, &end)) {
        gtk_text_buffer_remove_all_tags(buffer, &start, &end);
    }
}

// --- Autocompletion Functions ---

static const gchar* c_standard_libraries[] = {
    "stdio.h", "stdlib.h", "string.h", "math.h", "time.h",
    "ctype.h", "assert.h", "stddef.h", "stdint.h", "stdbool.h",
    "signal.h", "setjmp.h", "locale.h", "errno.h", "limits.h",
    "float.h", "stdarg.h", NULL
};

static void hide_completion_popup() {
    if (g_completion_popup) {
        gtk_widget_hide(g_completion_popup);
        g_is_autocompleting = FALSE;
    }
}

static void on_completion_row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer user_data) {
    if (!row) return;
    
    GtkWidget *label = gtk_bin_get_child(GTK_BIN(row));
    const gchar *text = gtk_label_get_text(GTK_LABEL(label));
    
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(g_textview_editor));
    GtkTextMark *insert_mark = gtk_text_buffer_get_insert(buffer);
    GtkTextIter iter;
    gtk_text_buffer_get_iter_at_mark(buffer, &iter, insert_mark);
    
    GtkTextIter start = iter;
    while (gtk_text_iter_backward_char(&start)) {
        gunichar ch = gtk_text_iter_get_char(&start);
        if (ch == '<') {
            gtk_text_iter_forward_char(&start);
            break;
        }
    }
    
    gtk_text_buffer_delete(buffer, &start, &iter);
    gtk_text_buffer_get_iter_at_mark(buffer, &iter, insert_mark);
    gtk_text_buffer_insert(buffer, &iter, text, -1);
    gtk_text_buffer_insert(buffer, &iter, ">", -1);
    
    hide_completion_popup();
}

static void show_completion_popup(const gchar **suggestions, gint count) {
    if (!g_completion_popup) {
        g_completion_popup = gtk_window_new(GTK_WINDOW_POPUP);
        gtk_window_set_type_hint(GTK_WINDOW(g_completion_popup), GDK_WINDOW_TYPE_HINT_COMBO);
        gtk_window_set_resizable(GTK_WINDOW(g_completion_popup), FALSE);
        
        GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        gtk_widget_set_size_request(scrolled, 250, 150);
        
        g_completion_listbox = gtk_list_box_new();
        g_signal_connect(g_completion_listbox, "row-activated", G_CALLBACK(on_completion_row_activated), NULL);
        
        gtk_container_add(GTK_CONTAINER(scrolled), g_completion_listbox);
        gtk_container_add(GTK_CONTAINER(g_completion_popup), scrolled);
    }
    
    gtk_container_foreach(GTK_CONTAINER(g_completion_listbox), (GtkCallback)gtk_widget_destroy, NULL);
    
    for (gint i = 0; i < count && suggestions[i] != NULL; i++) {
        GtkWidget *label = gtk_label_new(suggestions[i]);
        gtk_widget_set_halign(label, GTK_ALIGN_START);
        gtk_widget_set_margin_start(label, 5);
        gtk_widget_set_margin_end(label, 5);
        gtk_widget_set_margin_top(label, 3);
        gtk_widget_set_margin_bottom(label, 3);
        gtk_list_box_insert(GTK_LIST_BOX(g_completion_listbox), label, -1);
    }
    
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(g_textview_editor));
    GtkTextMark *insert_mark = gtk_text_buffer_get_insert(buffer);
    GtkTextIter iter;
    gtk_text_buffer_get_iter_at_mark(buffer, &iter, insert_mark);
    
    GdkRectangle rect;
    gtk_text_view_get_iter_location(GTK_TEXT_VIEW(g_textview_editor), &iter, &rect);
    
    gint x, y;
    gtk_text_view_buffer_to_window_coords(GTK_TEXT_VIEW(g_textview_editor), GTK_TEXT_WINDOW_WIDGET, rect.x, rect.y, &x, &y);
    
    GdkWindow *window = gtk_widget_get_window(g_textview_editor);
    gint win_x, win_y;
    gdk_window_get_origin(window, &win_x, &win_y);
    
    gtk_window_move(GTK_WINDOW(g_completion_popup), win_x + x, win_y + y + 20);
    gtk_widget_show_all(g_completion_popup);
    g_is_autocompleting = TRUE;
}

static gchar* get_word_before_cursor(GtkTextBuffer *buffer, GtkTextIter *cursor_iter) {
    GtkTextIter start = *cursor_iter;
    while (gtk_text_iter_backward_char(&start)) {
        gunichar ch = gtk_text_iter_get_char(&start);
        if (!g_unichar_isalnum(ch) && ch != '_' && ch != '<' && ch != '#') {
            gtk_text_iter_forward_char(&start);
            break;
        }
    }
    return gtk_text_buffer_get_text(buffer, &start, cursor_iter, FALSE);
}

static void check_and_show_completions(GtkTextBuffer *buffer) {
    GtkTextMark *insert_mark = gtk_text_buffer_get_insert(buffer);
    GtkTextIter iter;
    gtk_text_buffer_get_iter_at_mark(buffer, &iter, insert_mark);
    
    gchar *word = get_word_before_cursor(buffer, &iter);
    
    if (word && g_str_has_prefix(word, "#include<")) {
        const gchar *partial = word + 9;
        GPtrArray *matches = g_ptr_array_new();
        for (int i = 0; c_standard_libraries[i] != NULL; i++) {
            if (g_str_has_prefix(c_standard_libraries[i], partial)) {
                g_ptr_array_add(matches, (gpointer)c_standard_libraries[i]);
            }
        }
        if (matches->len > 0) {
            g_ptr_array_add(matches, NULL);
            show_completion_popup((const gchar**)matches->pdata, matches->len);
        } else {
            hide_completion_popup();
        }
        g_ptr_array_free(matches, TRUE);
    } else {
        hide_completion_popup();
    }
    g_free(word);
}

static gboolean auto_close_brace_idle(gpointer user_data) {
    GtkTextBuffer *buffer = GTK_TEXT_BUFFER(user_data);
    GtkTextMark *insert_mark = gtk_text_buffer_get_insert(buffer);
    GtkTextIter iter;
    gtk_text_buffer_get_iter_at_mark(buffer, &iter, insert_mark);
    gtk_text_buffer_insert(buffer, &iter, "}", -1);
    gtk_text_iter_backward_char(&iter);
    gtk_text_buffer_place_cursor(buffer, &iter);
    return FALSE;
}

static gboolean auto_close_paren_idle(gpointer user_data) {
    GtkTextBuffer *buffer = GTK_TEXT_BUFFER(user_data);
    GtkTextMark *insert_mark = gtk_text_buffer_get_insert(buffer);
    GtkTextIter iter;
    gtk_text_buffer_get_iter_at_mark(buffer, &iter, insert_mark);
    gtk_text_buffer_insert(buffer, &iter, ")", -1);
    gtk_text_iter_backward_char(&iter);
    gtk_text_buffer_place_cursor(buffer, &iter);
    return FALSE;
}

static gboolean auto_close_bracket_idle(gpointer user_data) {
    GtkTextBuffer *buffer = GTK_TEXT_BUFFER(user_data);
    GtkTextMark *insert_mark = gtk_text_buffer_get_insert(buffer);
    GtkTextIter iter;
    gtk_text_buffer_get_iter_at_mark(buffer, &iter, insert_mark);
    gtk_text_buffer_insert(buffer, &iter, "]", -1);
    gtk_text_iter_backward_char(&iter);
    gtk_text_buffer_place_cursor(buffer, &iter);
    return FALSE;
}

static gboolean auto_close_quote_idle(gpointer user_data) {
    GtkTextBuffer *buffer = GTK_TEXT_BUFFER(user_data);
    GtkTextMark *insert_mark = gtk_text_buffer_get_insert(buffer);
    GtkTextIter iter;
    gtk_text_buffer_get_iter_at_mark(buffer, &iter, insert_mark);
    gtk_text_buffer_insert(buffer, &iter, "\"", -1);
    gtk_text_iter_backward_char(&iter);
    gtk_text_buffer_place_cursor(buffer, &iter);
    return FALSE;
}

static gboolean auto_close_single_quote_idle(gpointer user_data) {
    GtkTextBuffer *buffer = GTK_TEXT_BUFFER(user_data);
    GtkTextMark *insert_mark = gtk_text_buffer_get_insert(buffer);
    GtkTextIter iter;
    gtk_text_buffer_get_iter_at_mark(buffer, &iter, insert_mark);
    gtk_text_buffer_insert(buffer, &iter, "'", -1);
    gtk_text_iter_backward_char(&iter);
    gtk_text_buffer_place_cursor(buffer, &iter);
    return FALSE;
}

static gboolean on_editor_key_press(GtkWidget *widget, GdkEventKey *event, gpointer user_data) {
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(widget));
    
    if (g_is_autocompleting) {
        if (event->keyval == GDK_KEY_Escape) {
            hide_completion_popup();
            return TRUE;
        } else if (event->keyval == GDK_KEY_Tab || event->keyval == GDK_KEY_ISO_Left_Tab) {
            GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(g_completion_listbox));
            if (!row) row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(g_completion_listbox), 0);
            if (row) on_completion_row_activated(GTK_LIST_BOX(g_completion_listbox), row, NULL);
            return TRUE;
        } else if (event->keyval == GDK_KEY_Return || event->keyval == GDK_KEY_KP_Enter) {
            hide_completion_popup();
            return FALSE;
        } else if (event->keyval == GDK_KEY_Down) {
            GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(g_completion_listbox));
            if (row) {
                gint index = gtk_list_box_row_get_index(row);
                GtkListBoxRow *next = gtk_list_box_get_row_at_index(GTK_LIST_BOX(g_completion_listbox), index + 1);
                if (next) gtk_list_box_select_row(GTK_LIST_BOX(g_completion_listbox), next);
            } else {
                GtkListBoxRow *first = gtk_list_box_get_row_at_index(GTK_LIST_BOX(g_completion_listbox), 0);
                if (first) gtk_list_box_select_row(GTK_LIST_BOX(g_completion_listbox), first);
            }
            return TRUE;
        } else if (event->keyval == GDK_KEY_Up) {
            GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(g_completion_listbox));
            if (row) {
                gint index = gtk_list_box_row_get_index(row);
                if (index > 0) {
                    GtkListBoxRow *prev = gtk_list_box_get_row_at_index(GTK_LIST_BOX(g_completion_listbox), index - 1);
                    if (prev) gtk_list_box_select_row(GTK_LIST_BOX(g_completion_listbox), prev);
                }
            }
            return TRUE;
        } else if (event->keyval == GDK_KEY_BackSpace || event->keyval == GDK_KEY_space) {
            hide_completion_popup();
            return FALSE;
        }
    }
    
    if (event->keyval == GDK_KEY_braceleft) {
        g_idle_add(auto_close_brace_idle, buffer);
        return FALSE;
    } else if (event->keyval == GDK_KEY_parenleft) {
        g_idle_add(auto_close_paren_idle, buffer);
        return FALSE;
    } else if (event->keyval == GDK_KEY_bracketleft) {
        g_idle_add(auto_close_bracket_idle, buffer);
        return FALSE;
    } else if (event->keyval == GDK_KEY_quotedbl) {
        g_idle_add(auto_close_quote_idle, buffer);
        return FALSE;
    } else if (event->keyval == GDK_KEY_apostrophe) {
        g_idle_add(auto_close_single_quote_idle, buffer);
        return FALSE;
    }
    
    queue_cursor_update();
    return FALSE;
}

static gboolean on_buffer_changed_idle(gpointer user_data) {
    if (!g_is_updating_from_network) {
        GtkTextBuffer *buffer = GTK_TEXT_BUFFER(user_data);
        check_and_show_completions(buffer);
    }
    return FALSE;
}

static void on_buffer_changed(GtkTextBuffer *buffer, gpointer user_data) {
    g_idle_add(on_buffer_changed_idle, buffer);
}

// --- Interactive Code Execution Callbacks ---

static gboolean on_child_output_ready(GIOChannel *source, GIOCondition condition, gpointer data) {
    gchar buffer[4096];
    gsize bytes_read = 0;
    GError *error = NULL;
    GIOStatus status;
    
    while (TRUE) {
        status = g_io_channel_read_chars(source, buffer, sizeof(buffer) - 1, &bytes_read, &error);
        if (status == G_IO_STATUS_NORMAL && bytes_read > 0) {
            buffer[bytes_read] = '\0';
            append_terminal_output(buffer);
            
            if (g_server_connection && !g_is_updating_from_network) {
                GString *escaped = g_string_new("");
                for (gsize i = 0; i < bytes_read; i++) {
                    if (buffer[i] == '\n') {
                        g_string_append(escaped, "\\n");
                    } else if (buffer[i] == '\r') {
                        g_string_append(escaped, "\\r");
                    } else if (buffer[i] == ':') {
                        g_string_append(escaped, "\\:");
                    } else if (buffer[i] == '\\') {
                        g_string_append(escaped, "\\\\");
                    } else {
                        g_string_append_c(escaped, buffer[i]);
                    }
                }
                
                gchar *msg = g_strdup_printf("TERMINAL:%s\n", escaped->str);
                GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(g_server_connection));
                g_output_stream_write_async(out, msg, strlen(msg), G_PRIORITY_DEFAULT, NULL, NULL, NULL);
                g_free(msg);
                g_string_free(escaped, TRUE);
            }
        } else {
            break;
        }
        if (error) {
            g_error_free(error);
            error = NULL;
        }
    }
    
    if (condition & G_IO_HUP || condition & G_IO_ERR || status == G_IO_STATUS_EOF) {
        return FALSE;
    }
    return TRUE;
}

static void on_child_exit(GPid pid, int status, gpointer data) {
    g_spawn_close_pid(pid);
    g_child_pid = 0;
    
    if (g_stdout_watch_id > 0) {
        g_source_remove(g_stdout_watch_id);
        g_stdout_watch_id = 0;
    }
    if (g_stderr_watch_id > 0) {
        g_source_remove(g_stderr_watch_id);
        g_stderr_watch_id = 0;
    }
    if (g_stdout_channel) {
        on_child_output_ready(g_stdout_channel, G_IO_IN, NULL);
        g_io_channel_shutdown(g_stdout_channel, TRUE, NULL);
        g_io_channel_unref(g_stdout_channel);
        g_stdout_channel = NULL;
    }
    if (g_stderr_channel) {
        on_child_output_ready(g_stderr_channel, G_IO_IN, NULL);
        g_io_channel_shutdown(g_stderr_channel, TRUE, NULL);
        g_io_channel_unref(g_stderr_channel);
        g_stderr_channel = NULL;
    }
    if (g_stdin_channel) {
        g_io_channel_shutdown(g_stdin_channel, TRUE, NULL);
        g_io_channel_unref(g_stdin_channel);
        g_stdin_channel = NULL;
    }
    gtk_widget_set_sensitive(g_terminal_input_entry, FALSE);
    
    gchar *exit_msg;
    if (WIFEXITED(status)) {
        exit_msg = g_strdup_printf("\n--- Execution Finished (Exit Status: %d) ---\n", WEXITSTATUS(status));
    } else {
        exit_msg = g_strdup("\n--- Execution Finished (Terminated Abnormally) ---\n");
    }
    
    append_terminal_output(exit_msg);
    
    if (g_server_connection && !g_is_updating_from_network) {
        gchar *msg = g_strdup_printf("TERMINAL:%s\n", exit_msg);
        GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(g_server_connection));
        g_output_stream_write_async(out, msg, strlen(msg), G_PRIORITY_DEFAULT, NULL, NULL, NULL);
        g_free(msg);
    }
    
    g_free(exit_msg);
}

static void on_terminal_send_clicked(GtkWidget *widget, gpointer data) {
    const gchar *input_text = gtk_entry_get_text(GTK_ENTRY(g_terminal_input_entry));
    if (g_stdin_channel && g_child_pid != 0) {
        gchar *full_input = g_strconcat(input_text, "\n", NULL);
        g_io_channel_write_chars(g_stdin_channel, full_input, -1, NULL, NULL);
        g_io_channel_flush(g_stdin_channel, NULL);
        g_free(full_input);
        gtk_entry_set_text(GTK_ENTRY(g_terminal_input_entry), "");
        gchar *echo = g_strconcat("> ", input_text, "\n", NULL);
        append_terminal_output(echo);
        g_free(echo);
    } else {
        append_terminal_output("Error: Program is not running or input pipe is closed.\n");
    }
}

// --- Networking and Collaboration Functions ---

static void apply_remote_operation(const gchar *operation) {
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(g_textview_editor));
    gchar **parts = g_strsplit(operation, ":", 3);
    g_is_updating_from_network = TRUE;
    
    if (parts[0] && g_strcmp0(parts[0], "INSERT") == 0 && parts[1] && parts[2]) {
        gint pos = atoi(parts[1]);
        GtkTextIter iter;
        gtk_text_buffer_get_iter_at_offset(buffer, &iter, pos);
        
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
        
        gtk_text_buffer_insert(buffer, &iter, unescaped->str, -1);
        g_string_free(unescaped, TRUE);
    } else if (parts[0] && g_strcmp0(parts[0], "DELETE") == 0 && parts[1] && parts[2]) {
        GtkTextIter start_iter, end_iter;
        gtk_text_buffer_get_iter_at_offset(buffer, &start_iter, atoi(parts[1]));
        gtk_text_buffer_get_iter_at_offset(buffer, &end_iter, atoi(parts[2]));
        gtk_text_buffer_delete(buffer, &start_iter, &end_iter);
    }
    
    g_is_updating_from_network = FALSE;
    g_strfreev(parts);
}

static void send_operation_to_server(const gchar *operation) {
    if (g_server_connection) {
        gchar *full_message = g_strconcat(operation, "\n", NULL);
        GOutputStream *out_stream = g_io_stream_get_output_stream(G_IO_STREAM(g_server_connection));
        g_output_stream_write_async(out_stream, full_message, strlen(full_message), G_PRIORITY_DEFAULT, NULL, NULL, NULL);
        g_free(full_message);
    }
}

void on_local_text_inserted(GtkTextBuffer *buffer, GtkTextIter *location, gchar *text, gint len, gpointer user_data) {
    if (g_is_updating_from_network) return;
    
    gint pos = gtk_text_iter_get_offset(location) - len;
    
    GString *escaped = g_string_new("");
    for (gint i = 0; i < len; i++) {
        if (text[i] == '\n') {
            g_string_append(escaped, "\\n");
        } else if (text[i] == '\r') {
            g_string_append(escaped, "\\r");
        } else if (text[i] == '\t') {
            g_string_append(escaped, "\\t");
        } else if (text[i] == '\\') {
            g_string_append(escaped, "\\\\");
        } else if (text[i] == ':') {
            g_string_append(escaped, "\\:");
        } else {
            g_string_append_c(escaped, text[i]);
        }
    }
    
    gchar *op = g_strdup_printf("INSERT:%d:%s", pos, escaped->str);
    send_operation_to_server(op);
    g_free(op);
    g_string_free(escaped, TRUE);
}

void on_local_text_deleted(GtkTextBuffer *buffer, GtkTextIter *start, GtkTextIter *end, gpointer user_data) {
    if (g_is_updating_from_network) return;
    gint start_pos = gtk_text_iter_get_offset(start);
    gint end_pos = gtk_text_iter_get_offset(end);
    gchar *op = g_strdup_printf("DELETE:%d:%d", start_pos, end_pos);
    send_operation_to_server(op);
    g_free(op);
}

static void apply_remote_format(const gchar *format_data) {
    gchar **parts = g_strsplit(format_data, ":", 4);
    
    if (parts[0] && parts[1] && parts[2] && parts[3]) {
        GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(g_textview_editor));
        GtkTextIter start_iter, end_iter;
        gint start_pos = atoi(parts[2]);
        gint end_pos = atoi(parts[3]);
        
        gtk_text_buffer_get_iter_at_offset(buffer, &start_iter, start_pos);
        gtk_text_buffer_get_iter_at_offset(buffer, &end_iter, end_pos);
        
        g_is_updating_from_network = TRUE;
        
        if (g_strcmp0(parts[0], "bold") == 0) {
            gboolean apply = atoi(parts[1]) == 1;
            if (apply) gtk_text_buffer_apply_tag(buffer, g_tag_bold, &start_iter, &end_iter);
            else gtk_text_buffer_remove_tag(buffer, g_tag_bold, &start_iter, &end_iter);
        } else if (g_strcmp0(parts[0], "italic") == 0) {
            gboolean apply = atoi(parts[1]) == 1;
            if (apply) gtk_text_buffer_apply_tag(buffer, g_tag_italic, &start_iter, &end_iter);
            else gtk_text_buffer_remove_tag(buffer, g_tag_italic, &start_iter, &end_iter);
        } else if (g_strcmp0(parts[0], "underline") == 0) {
            gboolean apply = atoi(parts[1]) == 1;
            if (apply) gtk_text_buffer_apply_tag(buffer, g_tag_underline, &start_iter, &end_iter);
            else gtk_text_buffer_remove_tag(buffer, g_tag_underline, &start_iter, &end_iter);
        } else if (g_strcmp0(parts[0], "align_left") == 0) {
            gtk_text_buffer_remove_tag(buffer, g_tag_align_center, &start_iter, &end_iter);
            gtk_text_buffer_remove_tag(buffer, g_tag_align_right, &start_iter, &end_iter);
            gtk_text_buffer_apply_tag(buffer, g_tag_align_left, &start_iter, &end_iter);
        } else if (g_strcmp0(parts[0], "align_center") == 0) {
            gtk_text_buffer_remove_tag(buffer, g_tag_align_left, &start_iter, &end_iter);
            gtk_text_buffer_remove_tag(buffer, g_tag_align_right, &start_iter, &end_iter);
            gtk_text_buffer_apply_tag(buffer, g_tag_align_center, &start_iter, &end_iter);
        } else if (g_strcmp0(parts[0], "align_right") == 0) {
            gtk_text_buffer_remove_tag(buffer, g_tag_align_left, &start_iter, &end_iter);
            gtk_text_buffer_remove_tag(buffer, g_tag_align_center, &start_iter, &end_iter);
            gtk_text_buffer_apply_tag(buffer, g_tag_align_right, &start_iter, &end_iter);
        } else if (g_strcmp0(parts[0], "color") == 0) {
            guint r, g, b;
            if (sscanf(parts[1], "#%02x%02x%02x", &r, &g, &b) == 3) {
                GdkRGBA color;
                color.red = r / 255.0;
                color.green = g / 255.0;
                color.blue = b / 255.0;
                color.alpha = 1.0;
                
                gchar *tag_name = g_strdup_printf("color_%d_%d_%d", r, g, b);
                GtkTextTag *tag = gtk_text_tag_table_lookup(gtk_text_buffer_get_tag_table(buffer), tag_name);
                if (!tag) {
                    tag = gtk_text_buffer_create_tag(buffer, tag_name, "foreground-rgba", &color, NULL);
                }
                gtk_text_buffer_apply_tag(buffer, tag, &start_iter, &end_iter);
                g_free(tag_name);
            }
        } else if (g_strcmp0(parts[0], "bgcolor") == 0) {
            guint r, g, b;
            if (sscanf(parts[1], "#%02x%02x%02x", &r, &g, &b) == 3) {
                GdkRGBA color;
                color.red = r / 255.0;
                color.green = g / 255.0;
                color.blue = b / 255.0;
                color.alpha = 1.0;
                
                gchar *tag_name = g_strdup_printf("bgcolor_%d_%d_%d", r, g, b);
                GtkTextTag *tag = gtk_text_tag_table_lookup(gtk_text_buffer_get_tag_table(buffer), tag_name);
                if (!tag) {
                    tag = gtk_text_buffer_create_tag(buffer, tag_name, "background-rgba", &color, NULL);
                }
                gtk_text_buffer_apply_tag(buffer, tag, &start_iter, &end_iter);
                g_free(tag_name);
            }
        } else if (g_strcmp0(parts[0], "size") == 0) {
            gint size = atoi(parts[1]);
            if (size > 0) {
                gchar *tag_name = g_strdup_printf("size_%d", size);
                GtkTextTag *tag = gtk_text_tag_table_lookup(gtk_text_buffer_get_tag_table(buffer), tag_name);
                if (!tag) {
                    tag = gtk_text_buffer_create_tag(buffer, tag_name, "size-points", (gdouble)size, NULL);
                }
                gtk_text_buffer_apply_tag(buffer, tag, &start_iter, &end_iter);
                g_free(tag_name);
            }
        }
        
        g_is_updating_from_network = FALSE;
    }
    
    g_strfreev(parts);
}

static void on_server_message_received(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    GDataInputStream *stream = G_DATA_INPUT_STREAM(source_object);
    GError *error = NULL;
    gchar *line = g_data_input_stream_read_line_finish(stream, res, NULL, &error);
    
    if (line == NULL || error) {
        append_terminal_output("\n--- Disconnected from server ---\n");
        if (g_server_connection) {
            g_io_stream_close(G_IO_STREAM(g_server_connection), NULL, NULL);
            g_object_unref(g_server_connection);
            g_server_connection = NULL;
        }
        if (error) g_error_free(error);
        return;
    }
    
    if (g_str_has_prefix(line, "USER_ID:")) {
        g_my_user_id = atoi(line + 8);
        gchar *msg = g_strdup_printf("Assigned User ID: %d\n", g_my_user_id);
        append_terminal_output(msg);
        g_free(msg);
    } else if (g_str_has_prefix(line, "INIT_DOCUMENT:")) {
        const gchar *content = line + 14;
        GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(g_textview_editor));
        g_is_updating_from_network = TRUE;
        gtk_text_buffer_set_text(buffer, content, -1);
        g_is_updating_from_network = FALSE;
    } else if (g_str_has_prefix(line, "USER_JOINED:")) {
        gchar **parts = g_strsplit(line + 12, ":", 3);
        if (parts[0] && parts[1] && parts[2]) {
            gint user_id = atoi(parts[0]);
            if (user_id != g_my_user_id) {
                add_remote_user(user_id, parts[1], atoi(parts[2]));
                gchar *msg = g_strdup_printf("User '%s' joined the session\n", parts[1]);
                append_terminal_output(msg);
                g_free(msg);
            }
        }
        g_strfreev(parts);
    } else if (g_str_has_prefix(line, "USER_LEFT:")) {
        gint user_id = atoi(line + 10);
        RemoteUser *user = find_remote_user(user_id);
        if (user) {
            gchar *msg = g_strdup_printf("User '%s' left the session\n", user->username);
            append_terminal_output(msg);
            g_free(msg);
            remove_remote_user(user_id);
        }
    } else if (g_str_has_prefix(line, "TEXT_OP:")) {
        gchar **parts = g_strsplit(line + 8, ":", 2);
        if (parts[0] && parts[1]) {
            gint user_id = atoi(parts[0]);
            if (user_id != g_my_user_id) {
                apply_remote_operation(parts[1]);
            }
        }
        g_strfreev(parts);
    } else if (g_str_has_prefix(line, "CURSOR:")) {
        gchar **parts = g_strsplit(line + 7, ":", 2);
        if (parts[0] && parts[1]) {
            gint user_id = atoi(parts[0]);
            gint position = atoi(parts[1]);
            if (user_id != g_my_user_id) {
                update_remote_cursor(user_id, position);
            }
        }
        g_strfreev(parts);
    } else if (g_str_has_prefix(line, "FORMAT:")) {
        apply_remote_format(line + 7);
    } else if (g_str_has_prefix(line, "CHAT:")) {
        gchar **parts = g_strsplit(line + 5, ":", 3);
        if (parts[0] && parts[1] && parts[2]) {
            GString *unescaped = g_string_new("");
            const gchar *p = parts[2];
            while (*p) {
                if (*p == '\\' && *(p+1)) {
                    p++;
                    if (*p == 'n') g_string_append_c(unescaped, '\n');
                    else if (*p == ':') g_string_append_c(unescaped, ':');
                    else if (*p == '\\') g_string_append_c(unescaped, '\\');
                    else g_string_append_c(unescaped, *p);
                    p++;
                } else {
                    g_string_append_c(unescaped, *p);
                    p++;
                }
            }
            display_chat_message(parts[1], unescaped->str);
            g_string_free(unescaped, TRUE);
        }
        g_strfreev(parts);
    } else if (g_str_has_prefix(line, "TERMINAL:")) {
        const gchar *output = line + 9;
        GString *unescaped = g_string_new("");
        const gchar *p = output;
        while (*p) {
            if (*p == '\\' && *(p+1)) {
                p++;
                if (*p == 'n') g_string_append_c(unescaped, '\n');
                else if (*p == 'r') g_string_append_c(unescaped, '\r');
                else if (*p == ':') g_string_append_c(unescaped, ':');
                else if (*p == '\\') g_string_append_c(unescaped, '\\');
                else g_string_append_c(unescaped, *p);
                p++;
            } else {
                g_string_append_c(unescaped, *p);
                p++;
            }
        }
        append_terminal_output(unescaped->str);
        g_string_free(unescaped, TRUE);
    } else if (g_str_has_prefix(line, "MEDIA_INSERT:")) {
        const gchar *media_data = line + 13;
        gchar **parts = g_strsplit(media_data, ":", 2);
        if (parts[0] && parts[1]) {
            // parts[0] = media type (image/video)
            // parts[1] = base64 encoded data or file path
            // For simplicity, we'll just add it to the gallery
            media_gallery_list = g_list_append(media_gallery_list, g_strdup(parts[1]));
            if (g_media_gallery_view) {
                GtkListStore *store = GTK_LIST_STORE(gtk_tree_view_get_model(GTK_TREE_VIEW(g_media_gallery_view)));
                GtkTreeIter iter;
                gtk_list_store_append(store, &iter);
                gtk_list_store_set(store, &iter, 0, parts[1], -1);
            }
        }
        g_strfreev(parts);
    }
    
    g_free(line);
    start_reading_from_server();
}

static void start_reading_from_server() {
    if (!g_server_connection || !g_server_input_stream) return;
    g_data_input_stream_read_line_async(g_server_input_stream, G_PRIORITY_DEFAULT, NULL, on_server_message_received, NULL);
}

static void on_join_attempt_finished(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    GSocketClient *client = G_SOCKET_CLIENT(source_object);
    GError *error = NULL;
    g_server_connection = g_socket_client_connect_to_host_finish(client, res, &error);
    
    if (error) {
        gchar *msg = g_strdup_printf("Could not connect to server: %s\n", error->message);
        append_terminal_output(msg);
        g_free(msg);
        g_error_free(error);
        
        GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(g_main_window), GTK_DIALOG_DESTROY_WITH_PARENT,
                                                   GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
                                                   "Failed to connect to server at %s:%d. Please check IP/Firewall.", g_server_host, DEFAULT_PORT);
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        return;
    }
    
    gtk_stack_set_visible_child_name(GTK_STACK(g_stack), "editor_view");
    gtk_window_set_default_size(GTK_WINDOW(g_main_window), 1200, 800);
    append_terminal_output("--- Successfully connected to server ---\n");
    
    gchar *login_msg = g_strdup_printf("LOGIN:%s\n", g_logged_in_user);
    GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(g_server_connection));
    g_output_stream_write_all(out, login_msg, strlen(login_msg), NULL, NULL, NULL);
    g_free(login_msg);
    
    GInputStream *in = g_io_stream_get_input_stream(G_IO_STREAM(g_server_connection));
    g_server_input_stream = g_data_input_stream_new(in);
    start_reading_from_server();
}

static void attempt_to_join_server() {
    if (g_logged_in_user) {
        g_version_history_file = g_strdup_printf("version_history_%s.txt", g_logged_in_user);
    }
    
    GSocketClient *client = g_socket_client_new();
    g_socket_client_connect_to_host_async(client, g_server_host, DEFAULT_PORT, NULL, on_join_attempt_finished, NULL);
    g_object_unref(client);
}

// --- Menu, Toolbar & Media Callback Implementations ---

static void menu_run_code(GtkWidget *widget, gpointer data) {
    if (g_child_pid != 0) {
        set_terminal_output("Error: Another program is already running.\n");
        return;
    }
    const char *TEMP_C_FILE = "/tmp/megathon_temp.c";
    const char *TEMP_EXEC_FILE = "/tmp/megathon_temp_exec";
    
    set_terminal_output("--- Code Execution Started ---\n");
    
    if (g_server_connection && !g_is_updating_from_network) {
        gchar *msg = g_strdup("TERMINAL:--- Code Execution Started ---\\n\n");
        GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(g_server_connection));
        g_output_stream_write_async(out, msg, strlen(msg), G_PRIORITY_DEFAULT, NULL, NULL, NULL);
        g_free(msg);
    }
    
    if (!save_content_to_file(TEMP_C_FILE, g_textview_editor)) {
        set_terminal_output("Error: Could not save content to temporary file.\n");
        return;
    }
    
    char command_compile[512];
    snprintf(command_compile, sizeof(command_compile), "gcc %s -o %s 2>&1", TEMP_C_FILE, TEMP_EXEC_FILE);
    FILE *compilation_pipe = popen(command_compile, "r");
    if (!compilation_pipe) {
        set_terminal_output("Error: Failed to open compilation pipe.\n");
        return;
    }
    char buffer[1024];
    while (fgets(buffer, sizeof(buffer), compilation_pipe) != NULL) {
        append_terminal_output(buffer);
        
        if (g_server_connection && !g_is_updating_from_network) {
            GString *escaped = g_string_new("");
            for (char *p = buffer; *p; p++) {
                if (*p == '\n') g_string_append(escaped, "\\n");
                else if (*p == '\r') g_string_append(escaped, "\\r");
                else if (*p == ':') g_string_append(escaped, "\\:");
                else if (*p == '\\') g_string_append(escaped, "\\\\");
                else g_string_append_c(escaped, *p);
            }
            gchar *msg = g_strdup_printf("TERMINAL:%s\n", escaped->str);
            GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(g_server_connection));
            g_output_stream_write_async(out, msg, strlen(msg), G_PRIORITY_DEFAULT, NULL, NULL, NULL);
            g_free(msg);
            g_string_free(escaped, TRUE);
        }
    }
    int compile_status = pclose(compilation_pipe);
    if (WEXITSTATUS(compile_status) != 0) {
        append_terminal_output("\n--- Compilation Failed ---\n");
        if (g_server_connection && !g_is_updating_from_network) {
            gchar *msg = g_strdup("TERMINAL:\\n--- Compilation Failed ---\\n\n");
            GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(g_server_connection));
            g_output_stream_write_async(out, msg, strlen(msg), G_PRIORITY_DEFAULT, NULL, NULL, NULL);
            g_free(msg);
        }
        return;
    }
    
    append_terminal_output("\n--- Compilation Successful. Running... ---\n");
    if (g_server_connection && !g_is_updating_from_network) {
        gchar *msg = g_strdup("TERMINAL:\\n--- Compilation Successful. Running... ---\\n\n");
        GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(g_server_connection));
        g_output_stream_write_async(out, msg, strlen(msg), G_PRIORITY_DEFAULT, NULL, NULL, NULL);
        g_free(msg);
    }
    
    gint stdin_fd, stdout_fd, stderr_fd;
    gchar *argv[] = {(gchar*)TEMP_EXEC_FILE, NULL};
    GError *error = NULL;
    
    gboolean success = g_spawn_async_with_pipes(NULL, argv, NULL, G_SPAWN_DO_NOT_REAP_CHILD,
        NULL, NULL, &g_child_pid, &stdin_fd, &stdout_fd, &stderr_fd, &error);
    
    if (!success) {
        gchar *msg = g_strdup_printf("Error: Failed to launch executable: %s\n", error ? error->message : "Unknown error");
        append_terminal_output(msg);
        g_free(msg);
        if (error) g_error_free(error);
        return;
    }
    
    g_stdout_channel = g_io_channel_unix_new(stdout_fd);
    g_stderr_channel = g_io_channel_unix_new(stderr_fd);
    g_stdin_channel = g_io_channel_unix_new(stdin_fd);
    
    g_io_channel_set_encoding(g_stdout_channel, NULL, NULL);
    g_io_channel_set_encoding(g_stderr_channel, NULL, NULL);
    g_io_channel_set_encoding(g_stdin_channel, NULL, NULL);
    
    g_io_channel_set_buffered(g_stdout_channel, FALSE);
    g_io_channel_set_buffered(g_stderr_channel, FALSE);
    g_io_channel_set_buffered(g_stdin_channel, FALSE);
    
    GIOFlags flags = g_io_channel_get_flags(g_stdout_channel);
    g_io_channel_set_flags(g_stdout_channel, flags | G_IO_FLAG_NONBLOCK, NULL);
    flags = g_io_channel_get_flags(g_stderr_channel);
    g_io_channel_set_flags(g_stderr_channel, flags | G_IO_FLAG_NONBLOCK, NULL);
    
    g_stdout_watch_id = g_io_add_watch(g_stdout_channel, G_IO_IN | G_IO_HUP | G_IO_ERR, on_child_output_ready, NULL);
    g_stderr_watch_id = g_io_add_watch(g_stderr_channel, G_IO_IN | G_IO_HUP | G_IO_ERR, on_child_output_ready, NULL);
    
    g_child_watch_add(g_child_pid, on_child_exit, NULL);
    gtk_widget_set_sensitive(g_terminal_input_entry, TRUE);
}

static void menu_debug_code(GtkWidget *widget, gpointer data) {
    append_terminal_output("Action: Debug Code (Not yet implemented - use `gdb` manually).\n");
}

static void open_file(GtkWidget *widget, gpointer textview) {
    GtkWidget *dialog;
    dialog = gtk_file_chooser_dialog_new("Open File", GTK_WINDOW(g_main_window),
                                         GTK_FILE_CHOOSER_ACTION_OPEN,
                                         "_Cancel", GTK_RESPONSE_CANCEL,
                                         "_Open", GTK_RESPONSE_ACCEPT, NULL);
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        gchar *content = NULL;
        if (g_file_get_contents(filename, &content, NULL, NULL)) {
            GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(g_textview_editor));
            
            // Clear the buffer first (this will send delete operations)
            GtkTextIter start, end;
            gtk_text_buffer_get_bounds(buffer, &start, &end);
            gtk_text_buffer_delete(buffer, &start, &end);
            
            // Set the new content (this will send insert operations)
            gtk_text_buffer_set_text(buffer, content, -1);
            
            g_free(content);
            append_terminal_output(g_strdup_printf("Opened file: %s\n", filename));
        }
        g_free(filename);
    }
    gtk_widget_destroy(dialog);
}

static void toggle_sidebar_visibility(int target_page) {
    if (!g_sidebar_notebook) return;
    if (g_sidebar_is_visible && gtk_notebook_get_current_page(GTK_NOTEBOOK(g_sidebar_notebook)) == target_page) {
        gtk_widget_hide(g_sidebar_notebook);
        g_sidebar_is_visible = FALSE;
    } else {
        gtk_notebook_set_current_page(GTK_NOTEBOOK(g_sidebar_notebook), target_page);
        gtk_widget_show(g_sidebar_notebook);
        g_sidebar_is_visible = TRUE;
    }
}

static void toggle_ai_chat_sidebar(GtkWidget *widget, gpointer data) {
    toggle_sidebar_visibility(0);
}

static void toggle_user_chat_sidebar(GtkWidget *widget, gpointer data) {
    toggle_sidebar_visibility(1);
}

static void open_file_with_system(const gchar *filepath) {
    if (filepath && g_file_test(filepath, G_FILE_TEST_EXISTS)) {
        gchar *cmd = g_strdup_printf("xdg-open '%s' &", filepath);
        system(cmd);
        g_free(cmd);
    }
}

static void update_media_gallery(GtkWidget *gallery_view) {
    if (!gallery_view) return;
    GtkListStore *store = GTK_LIST_STORE(gtk_tree_view_get_model(GTK_TREE_VIEW(gallery_view)));
    gtk_list_store_clear(store);
    for (GList *l = media_gallery_list; l != NULL; l = l->next) {
        GtkTreeIter iter;
        gtk_list_store_append(store, &iter);
        gtk_list_store_set(store, &iter, 0, (char *)l->data, -1);
    }
}

static void insert_multimedia(GtkWidget *widget, gpointer data) {
    GtkWidget *parent_window = gtk_widget_get_toplevel(GTK_WIDGET(widget));
    GtkWidget *dialog;
    gint response;

    #define RESPONSE_IMAGE 100
    #define RESPONSE_VIDEO 101

    dialog = gtk_dialog_new_with_buttons("Insert Media", GTK_WINDOW(parent_window),
                                         GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                         "_Cancel", GTK_RESPONSE_CANCEL, NULL);

    GtkWidget *button_image = gtk_button_new_with_label("Insert Image");
    GtkWidget *button_video = gtk_button_new_with_label("Insert Video");
    
    gtk_dialog_add_action_widget(GTK_DIALOG(dialog), button_image, RESPONSE_IMAGE);
    gtk_dialog_add_action_widget(GTK_DIALOG(dialog), button_video, RESPONSE_VIDEO);
    gtk_widget_show_all(gtk_dialog_get_content_area(GTK_DIALOG(dialog)));
    
    response = gtk_dialog_run(GTK_DIALOG(dialog));

    const char *media_type = NULL;
    if (response == RESPONSE_IMAGE) {
        media_type = "image";
    } else if (response == RESPONSE_VIDEO) {
        media_type = "video";
    }
    
    gtk_widget_destroy(dialog);

    if (media_type == NULL) return;

    GtkWidget *chooser = gtk_file_chooser_dialog_new("Select Media File", GTK_WINDOW(parent_window),
                                                     GTK_FILE_CHOOSER_ACTION_OPEN,
                                                     "_Cancel", GTK_RESPONSE_CANCEL,
                                                     "_Open", GTK_RESPONSE_ACCEPT, NULL);

    GtkFileFilter *filter = gtk_file_filter_new();
    if (g_strcmp0(media_type, "image") == 0) {
        gtk_file_filter_set_name(filter, "Images");
        gtk_file_filter_add_mime_type(filter, "image/png");
        gtk_file_filter_add_mime_type(filter, "image/jpeg");
        gtk_file_filter_add_mime_type(filter, "image/gif");
    } else if (g_strcmp0(media_type, "video") == 0) {
        gtk_file_filter_set_name(filter, "Videos");
        gtk_file_filter_add_mime_type(filter, "video/mp4");
        gtk_file_filter_add_mime_type(filter, "video/x-matroska");
        gtk_file_filter_add_mime_type(filter, "video/x-msvideo");
    }
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(chooser), filter);

    if (gtk_dialog_run(GTK_DIALOG(chooser)) == GTK_RESPONSE_ACCEPT) {
        char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));
        gchar *content_type = g_content_type_guess(filename, NULL, 0, NULL);

        GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(g_textview_editor));
        GtkTextIter iter;
        gtk_text_buffer_get_end_iter(buffer, &iter);

        media_gallery_list = g_list_append(media_gallery_list, g_strdup(filename));
        update_media_gallery(g_media_gallery_view);
        
        // Send media insert to server
        if (g_server_connection && !g_is_updating_from_network) {
            gchar *media_msg = g_strdup_printf("MEDIA_INSERT:%s:%s\n", media_type, filename);
            GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(g_server_connection));
            g_output_stream_write_async(out, media_msg, strlen(media_msg), G_PRIORITY_DEFAULT, NULL, NULL, NULL);
            g_free(media_msg);
        }

        if (g_str_has_prefix(content_type, "image/")) {
            gtk_text_buffer_insert(buffer, &iter, "\n[Image Section]\n", -1);
            GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file_at_size(filename, 200, 150, NULL);
            if (pixbuf) {
                GtkTextChildAnchor *anchor = gtk_text_buffer_create_child_anchor(buffer, &iter);
                GtkWidget *image_widget = gtk_image_new_from_pixbuf(pixbuf);
                gtk_text_view_add_child_at_anchor(GTK_TEXT_VIEW(g_textview_editor), image_widget, anchor);
                gtk_widget_show(image_widget);
                g_object_unref(pixbuf);
            }
            gtk_text_buffer_insert(buffer, &iter, "\n[End of Image]\n", -1);
        } else if (g_str_has_prefix(content_type, "video/")) {
            gtk_text_buffer_insert(buffer, &iter, "\n[Video Section]\n", -1);
            GtkTextChildAnchor *anchor = gtk_text_buffer_create_child_anchor(buffer, &iter);
            GtkWidget *play_button = gtk_button_new_with_label("Play Video (Opens Externally)");
            g_signal_connect_swapped(play_button, "clicked", G_CALLBACK(open_file_with_system), g_strdup(filename));
            gtk_text_view_add_child_at_anchor(GTK_TEXT_VIEW(g_textview_editor), play_button, anchor);
            gtk_widget_show(play_button);
            gtk_text_buffer_insert(buffer, &iter, "\n[End of Video]\n", -1);
        }
        g_free(content_type);
        g_free(filename);
    }
    gtk_widget_destroy(chooser);
}

static void sign_out_user() {
    if (g_server_connection) {
        g_io_stream_close(G_IO_STREAM(g_server_connection), NULL, NULL);
        g_object_unref(g_server_connection);
        g_server_connection = NULL;
    }
    if (g_server_input_stream) {
        g_object_unref(g_server_input_stream);
        g_server_input_stream = NULL;
    }
    
    g_free(g_logged_in_user);
    g_logged_in_user = NULL;
    g_my_user_id = -1;
    
    gtk_window_set_default_size(GTK_WINDOW(g_main_window), 300, 200);
    gtk_stack_set_visible_child_name(GTK_STACK(g_stack), "login_view");
}

static void quit_app(GtkWidget *widget, gpointer data) {
    if (g_child_pid != 0) {
        kill(g_child_pid, SIGTERM);
    }
    sign_out_user();
    if (media_gallery_list) g_list_free_full(media_gallery_list, g_free);
    g_application_quit(G_APPLICATION(g_app));
}

// --- UI Creation ---

static void on_login_clicked(GtkWidget *widget, gpointer data) {
    GtkWidget **entries = (GtkWidget **)data;
    const gchar *username = gtk_entry_get_text(GTK_ENTRY(entries[0]));
    const gchar *password = gtk_entry_get_text(GTK_ENTRY(entries[1]));
    if (check_user_credentials(username, password)) {
        g_logged_in_user = g_strdup(username);
        attempt_to_join_server();
    } else {
        GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(g_main_window), GTK_DIALOG_DESTROY_WITH_PARENT,
                                                   GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
                                                   "Invalid username or password.");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
    }
}

static void on_register_clicked(GtkWidget *widget, gpointer data) {
    GtkWidget **entries = (GtkWidget **)data;
    const gchar *username = gtk_entry_get_text(GTK_ENTRY(entries[0]));
    const gchar *password = gtk_entry_get_text(GTK_ENTRY(entries[1]));
    if (register_user(username, password)) {
        GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(g_main_window), GTK_DIALOG_DESTROY_WITH_PARENT,
                                                   GTK_MESSAGE_INFO, GTK_BUTTONS_CLOSE,
                                                   "Registration successful!");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
    } else {
        GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(g_main_window), GTK_DIALOG_DESTROY_WITH_PARENT,
                                                   GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
                                                   "Registration failed. User may already exist.");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
    }
}

static GtkWidget* create_login_view() {
    GtkWidget *grid = gtk_grid_new();
    gtk_widget_set_halign(grid, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(grid, GTK_ALIGN_CENTER);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);

    static GtkWidget *entries[2];
    entries[0] = gtk_entry_new();
    entries[1] = gtk_entry_new();
    gtk_entry_set_visibility(GTK_ENTRY(entries[1]), FALSE);

    GtkWidget *login_btn = gtk_button_new_with_label("Login");
    GtkWidget *register_btn = gtk_button_new_with_label("Register");

    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Username:"), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), entries[0], 1, 0, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Password:"), 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), entries[1], 1, 1, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), login_btn, 1, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), register_btn, 2, 2, 1, 1);
    
    g_signal_connect(login_btn, "clicked", G_CALLBACK(on_login_clicked), entries);
    g_signal_connect(register_btn, "clicked", G_CALLBACK(on_register_clicked), entries);

    return grid;
}

static GtkWidget* create_editor_view() {
    GtkWidget *vbox_main = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    GtkWidget *menubar = gtk_menu_bar_new();
    gtk_box_pack_start(GTK_BOX(vbox_main), menubar, FALSE, FALSE, 0);

    GtkWidget *filemenu = gtk_menu_new();
    GtkWidget *file_item = gtk_menu_item_new_with_label("File");
    GtkWidget *open_item = gtk_menu_item_new_with_label("Open");
    GtkWidget *save_item = gtk_menu_item_new_with_label("Save (Version History)");
    GtkWidget *save_as_item = gtk_menu_item_new_with_label("Save As (Local)");
    GtkWidget *signout_item = gtk_menu_item_new_with_label("Sign Out");
    GtkWidget *quit_item = gtk_menu_item_new_with_label("Quit");
    gtk_menu_shell_append(GTK_MENU_SHELL(filemenu), open_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(filemenu), save_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(filemenu), save_as_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(filemenu), gtk_separator_menu_item_new());
    gtk_menu_shell_append(GTK_MENU_SHELL(filemenu), signout_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(filemenu), quit_item);
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(file_item), filemenu);
    gtk_menu_shell_append(GTK_MENU_SHELL(menubar), file_item);

    GtkWidget *code_menu = gtk_menu_new();
    GtkWidget *code_item = gtk_menu_item_new_with_label("Code");
    GtkWidget *run_item = gtk_menu_item_new_with_label("Run Executable");
    GtkWidget *debug_item = gtk_menu_item_new_with_label("Debug Code");
    gtk_menu_shell_append(GTK_MENU_SHELL(code_menu), run_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(code_menu), debug_item);
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(code_item), code_menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(menubar), code_item);

    GtkWidget *toolbar = gtk_toolbar_new();
    gtk_toolbar_set_style(GTK_TOOLBAR(toolbar), GTK_TOOLBAR_BOTH);
    gtk_box_pack_start(GTK_BOX(vbox_main), toolbar, FALSE, FALSE, 0);

    GtkToolItem *button_undo = gtk_tool_button_new(NULL, "← Back");
    GtkToolItem *button_open = gtk_tool_button_new(NULL, "Open");
    GtkToolItem *button_save = gtk_tool_button_new(NULL, "Save");
    GtkToolItem *button_save_as = gtk_tool_button_new(NULL, "Save As");
    GtkToolItem *button_run = gtk_tool_button_new(NULL, "Run");
    GtkToolItem *button_image = gtk_tool_button_new(NULL, "Insert Media");
    GtkToolItem *button_version = gtk_tool_button_new(NULL, "History");
    GtkToolItem *button_ai_chat = gtk_tool_button_new(NULL, "AI Chat");
    GtkToolItem *button_user_chat = gtk_tool_button_new(NULL, "User Chat");
    
    gtk_toolbar_insert(GTK_TOOLBAR(toolbar), button_undo, -1);
    gtk_toolbar_insert(GTK_TOOLBAR(toolbar), gtk_separator_tool_item_new(), -1);
    gtk_toolbar_insert(GTK_TOOLBAR(toolbar), button_open, -1);
    gtk_toolbar_insert(GTK_TOOLBAR(toolbar), button_save, -1);
    gtk_toolbar_insert(GTK_TOOLBAR(toolbar), button_save_as, -1);
    gtk_toolbar_insert(GTK_TOOLBAR(toolbar), gtk_separator_tool_item_new(), -1);
    gtk_toolbar_insert(GTK_TOOLBAR(toolbar), button_run, -1);
    gtk_toolbar_insert(GTK_TOOLBAR(toolbar), gtk_separator_tool_item_new(), -1);
    gtk_toolbar_insert(GTK_TOOLBAR(toolbar), button_image, -1);
    gtk_toolbar_insert(GTK_TOOLBAR(toolbar), button_version, -1);
    gtk_toolbar_insert(GTK_TOOLBAR(toolbar), gtk_separator_tool_item_new(), -1);
    gtk_toolbar_insert(GTK_TOOLBAR(toolbar), button_ai_chat, -1);
    gtk_toolbar_insert(GTK_TOOLBAR(toolbar), button_user_chat, -1);

    GtkWidget *format_toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_widget_set_margin_start(format_toolbar, 5);
    gtk_widget_set_margin_end(format_toolbar, 5);
    gtk_widget_set_margin_top(format_toolbar, 5);
    gtk_widget_set_margin_bottom(format_toolbar, 5);
    gtk_box_pack_start(GTK_BOX(vbox_main), format_toolbar, FALSE, FALSE, 0);
    
    GtkWidget *font_size_label = gtk_label_new("Font Size:");
    gtk_box_pack_start(GTK_BOX(format_toolbar), font_size_label, FALSE, FALSE, 0);
    
    GtkWidget *font_size_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(font_size_combo), "8");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(font_size_combo), "10");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(font_size_combo), "12");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(font_size_combo), "14");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(font_size_combo), "16");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(font_size_combo), "18");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(font_size_combo), "20");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(font_size_combo), "24");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(font_size_combo), "28");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(font_size_combo), "32");
    gtk_combo_box_set_active(GTK_COMBO_BOX(font_size_combo), 2);
    g_signal_connect(font_size_combo, "changed", G_CALLBACK(on_font_size_changed), NULL);
    gtk_box_pack_start(GTK_BOX(format_toolbar), font_size_combo, FALSE, FALSE, 5);
    
    GtkWidget *sep1 = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
    gtk_box_pack_start(GTK_BOX(format_toolbar), sep1, FALSE, FALSE, 5);
    
    GtkWidget *bold_btn = gtk_toggle_button_new_with_label("B");
    PangoAttrList *bold_attrs = pango_attr_list_new();
    pango_attr_list_insert(bold_attrs, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
    gtk_label_set_attributes(GTK_LABEL(gtk_bin_get_child(GTK_BIN(bold_btn))), bold_attrs);
    pango_attr_list_unref(bold_attrs);
    g_signal_connect(bold_btn, "toggled", G_CALLBACK(on_bold_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(format_toolbar), bold_btn, FALSE, FALSE, 0);
    
    GtkWidget *italic_btn = gtk_toggle_button_new_with_label("I");
    PangoAttrList *italic_attrs = pango_attr_list_new();
    pango_attr_list_insert(italic_attrs, pango_attr_style_new(PANGO_STYLE_ITALIC));
    gtk_label_set_attributes(GTK_LABEL(gtk_bin_get_child(GTK_BIN(italic_btn))), italic_attrs);
    pango_attr_list_unref(italic_attrs);
    g_signal_connect(italic_btn, "toggled", G_CALLBACK(on_italic_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(format_toolbar), italic_btn, FALSE, FALSE, 0);
    
    GtkWidget *underline_btn = gtk_toggle_button_new_with_label("U");
    PangoAttrList *underline_attrs = pango_attr_list_new();
    pango_attr_list_insert(underline_attrs, pango_attr_underline_new(PANGO_UNDERLINE_SINGLE));
    gtk_label_set_attributes(GTK_LABEL(gtk_bin_get_child(GTK_BIN(underline_btn))), underline_attrs);
    pango_attr_list_unref(underline_attrs);
    g_signal_connect(underline_btn, "toggled", G_CALLBACK(on_underline_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(format_toolbar), underline_btn, FALSE, FALSE, 0);
    
    GtkWidget *sep2 = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
    gtk_box_pack_start(GTK_BOX(format_toolbar), sep2, FALSE, FALSE, 5);
    
    GtkWidget *align_left_btn = gtk_button_new_with_label("Left");
    g_signal_connect(align_left_btn, "clicked", G_CALLBACK(on_align_left_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(format_toolbar), align_left_btn, FALSE, FALSE, 0);
    
    GtkWidget *align_center_btn = gtk_button_new_with_label("Center");
    g_signal_connect(align_center_btn, "clicked", G_CALLBACK(on_align_center_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(format_toolbar), align_center_btn, FALSE, FALSE, 0);
    
    GtkWidget *align_right_btn = gtk_button_new_with_label("Right");
    g_signal_connect(align_right_btn, "clicked", G_CALLBACK(on_align_right_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(format_toolbar), align_right_btn, FALSE, FALSE, 0);
    
    GtkWidget *sep3 = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
    gtk_box_pack_start(GTK_BOX(format_toolbar), sep3, FALSE, FALSE, 5);
    
    GtkWidget *text_color_btn = gtk_button_new_with_label("Text Color");
    g_signal_connect(text_color_btn, "clicked", G_CALLBACK(on_text_color_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(format_toolbar), text_color_btn, FALSE, FALSE, 0);
    
    GtkWidget *bg_color_btn = gtk_button_new_with_label("BG Color");
    g_signal_connect(bg_color_btn, "clicked", G_CALLBACK(on_bg_color_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(format_toolbar), bg_color_btn, FALSE, FALSE, 0);
    
    GtkWidget *sep4 = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
    gtk_box_pack_start(GTK_BOX(format_toolbar), sep4, FALSE, FALSE, 5);
    
    GtkWidget *clear_format_btn = gtk_button_new_with_label("Clear Format");
    g_signal_connect(clear_format_btn, "clicked", G_CALLBACK(on_clear_formatting_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(format_toolbar), clear_format_btn, FALSE, FALSE, 0);

    GtkWidget *hbox_workspace = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(vbox_main), hbox_workspace, TRUE, TRUE, 0);

    GtkWidget *vpaned = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
    gtk_paned_set_position(GTK_PANED(vpaned), 550);
    gtk_box_pack_start(GTK_BOX(hbox_workspace), vpaned, TRUE, TRUE, 0);
    
    GtkWidget *sw_editor = gtk_scrolled_window_new(NULL, NULL);
    gtk_paned_add1(GTK_PANED(vpaned), sw_editor);
    g_textview_editor = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(g_textview_editor), GTK_WRAP_WORD);
    gtk_container_add(GTK_CONTAINER(sw_editor), g_textview_editor);
    
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(g_textview_editor));
    
    g_tag_bold = gtk_text_buffer_create_tag(buffer, "bold", "weight", PANGO_WEIGHT_BOLD, NULL);
    g_tag_italic = gtk_text_buffer_create_tag(buffer, "italic", "style", PANGO_STYLE_ITALIC, NULL);
    g_tag_underline = gtk_text_buffer_create_tag(buffer, "underline", "underline", PANGO_UNDERLINE_SINGLE, NULL);
    g_tag_align_left = gtk_text_buffer_create_tag(buffer, "align_left", "justification", GTK_JUSTIFY_LEFT, NULL);
    g_tag_align_center = gtk_text_buffer_create_tag(buffer, "align_center", "justification", GTK_JUSTIFY_CENTER, NULL);
    g_tag_align_right = gtk_text_buffer_create_tag(buffer, "align_right", "justification", GTK_JUSTIFY_RIGHT, NULL);
    
    g_signal_connect(g_textview_editor, "key-press-event", G_CALLBACK(on_editor_key_press), NULL);
    g_signal_connect(buffer, "changed", G_CALLBACK(on_buffer_changed), NULL);
    g_signal_connect(buffer, "insert-text", G_CALLBACK(on_local_text_inserted), NULL);
    g_signal_connect(buffer, "delete-range", G_CALLBACK(on_local_text_deleted), NULL);
    
    GtkWidget *vbox_terminal_area = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_paned_add2(GTK_PANED(vpaned), vbox_terminal_area);
    GtkWidget *sw_terminal = gtk_scrolled_window_new(NULL, NULL);
    gtk_widget_set_vexpand(sw_terminal, TRUE);
    gtk_box_pack_start(GTK_BOX(vbox_terminal_area), sw_terminal, TRUE, TRUE, 0);
    g_textview_terminal = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(g_textview_terminal), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(g_textview_terminal), GTK_WRAP_WORD);
    gtk_container_add(GTK_CONTAINER(sw_terminal), g_textview_terminal);

    GtkWidget *hbox_terminal_input = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_box_pack_start(GTK_BOX(vbox_terminal_area), hbox_terminal_input, FALSE, FALSE, 0);
    g_terminal_input_entry = gtk_entry_new();
    gtk_widget_set_sensitive(g_terminal_input_entry, FALSE);
    GtkWidget *send_button = gtk_button_new_with_label("Send Input");
    gtk_box_pack_start(GTK_BOX(hbox_terminal_input), gtk_label_new("Input:"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox_terminal_input), g_terminal_input_entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(hbox_terminal_input), send_button, FALSE, FALSE, 0);

    g_sidebar_notebook = gtk_notebook_new();
    gtk_widget_hide(g_sidebar_notebook);
    gtk_widget_set_size_request(g_sidebar_notebook, 300, -1);
    gtk_box_pack_start(GTK_BOX(hbox_workspace), g_sidebar_notebook, FALSE, FALSE, 0);
    
    // AI Chat Tab
    GtkWidget *ai_chat_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_widget_set_margin_start(ai_chat_vbox, 5);
    gtk_widget_set_margin_end(ai_chat_vbox, 5);
    gtk_widget_set_margin_top(ai_chat_vbox, 5);
    gtk_widget_set_margin_bottom(ai_chat_vbox, 5);
    
    GtkWidget *ai_chat_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_widget_set_vexpand(ai_chat_scroll, TRUE);
    g_ai_chat_textview = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(g_ai_chat_textview), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(g_ai_chat_textview), GTK_WRAP_WORD);
    gtk_container_add(GTK_CONTAINER(ai_chat_scroll), g_ai_chat_textview);
    gtk_box_pack_start(GTK_BOX(ai_chat_vbox), ai_chat_scroll, TRUE, TRUE, 0);
    
    GtkWidget *ai_chat_input_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    g_ai_chat_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(g_ai_chat_entry), "Ask Gemini AI...");
    g_ai_chat_send_button = gtk_button_new_with_label("Send");
    
    gtk_box_pack_start(GTK_BOX(ai_chat_input_box), g_ai_chat_entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(ai_chat_input_box), g_ai_chat_send_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ai_chat_vbox), ai_chat_input_box, FALSE, FALSE, 0);
    
    g_signal_connect(g_ai_chat_send_button, "clicked", G_CALLBACK(on_ai_chat_send_clicked), NULL);
    g_signal_connect(g_ai_chat_entry, "activate", G_CALLBACK(on_ai_chat_send_clicked), NULL);
    
    gtk_notebook_append_page(GTK_NOTEBOOK(g_sidebar_notebook), ai_chat_vbox, gtk_label_new("AI Assistant"));

    // User Chat Tab
    GtkWidget *chat_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_widget_set_margin_start(chat_vbox, 5);
    gtk_widget_set_margin_end(chat_vbox, 5);
    gtk_widget_set_margin_top(chat_vbox, 5);
    gtk_widget_set_margin_bottom(chat_vbox, 5);
    
    GtkWidget *chat_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_widget_set_vexpand(chat_scroll, TRUE);
    g_chat_textview = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(g_chat_textview), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(g_chat_textview), GTK_WRAP_WORD);
    gtk_container_add(GTK_CONTAINER(chat_scroll), g_chat_textview);
    gtk_box_pack_start(GTK_BOX(chat_vbox), chat_scroll, TRUE, TRUE, 0);
    
    GtkWidget *chat_input_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    g_chat_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(g_chat_entry), "Type your message...");
    GtkWidget *chat_send_btn = gtk_button_new_with_label("Send");
    
    gtk_box_pack_start(GTK_BOX(chat_input_box), g_chat_entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(chat_input_box), chat_send_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(chat_vbox), chat_input_box, FALSE, FALSE, 0);
    
    g_signal_connect(chat_send_btn, "clicked", G_CALLBACK(on_chat_send_clicked), NULL);
    g_signal_connect(g_chat_entry, "activate", G_CALLBACK(on_chat_send_clicked), NULL);
    
    gtk_notebook_append_page(GTK_NOTEBOOK(g_sidebar_notebook), chat_vbox, gtk_label_new("User Chat"));
    
    // Media Gallery Tab
    GtkWidget *gallery_sw = gtk_scrolled_window_new(NULL, NULL);
    GtkListStore *store = gtk_list_store_new(1, G_TYPE_STRING);
    g_media_gallery_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    g_object_unref(store);
    GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes("Inserted Media", renderer, "text", 0, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(g_media_gallery_view), column);
    gtk_container_add(GTK_CONTAINER(gallery_sw), g_media_gallery_view);
    gtk_notebook_append_page(GTK_NOTEBOOK(g_sidebar_notebook), gallery_sw, gtk_label_new("Media"));

    g_signal_connect(open_item, "activate", G_CALLBACK(open_file), g_textview_editor);
    g_signal_connect(save_item, "activate", G_CALLBACK(on_save_clicked), NULL);
    g_signal_connect(save_as_item, "activate", G_CALLBACK(on_save_as_clicked), NULL);
    g_signal_connect(signout_item, "activate", G_CALLBACK(sign_out_user), NULL);
    g_signal_connect(quit_item, "activate", G_CALLBACK(quit_app), NULL);
    g_signal_connect(run_item, "activate", G_CALLBACK(menu_run_code), NULL);
    g_signal_connect(debug_item, "activate", G_CALLBACK(menu_debug_code), NULL);

    g_signal_connect(button_undo, "clicked", G_CALLBACK(on_undo_version_clicked), NULL);
    g_signal_connect(button_open, "clicked", G_CALLBACK(open_file), g_textview_editor);
    g_signal_connect(button_save, "clicked", G_CALLBACK(on_save_clicked), NULL);
    g_signal_connect(button_save_as, "clicked", G_CALLBACK(on_save_as_clicked), NULL);
    g_signal_connect(button_run, "clicked", G_CALLBACK(menu_run_code), NULL);
    g_signal_connect(button_image, "clicked", G_CALLBACK(insert_multimedia), NULL);
    g_signal_connect(button_version, "clicked", G_CALLBACK(show_version_history_dialog), NULL);
    g_signal_connect(button_ai_chat, "clicked", G_CALLBACK(toggle_ai_chat_sidebar), NULL);
    g_signal_connect(button_user_chat, "clicked", G_CALLBACK(toggle_user_chat_sidebar), NULL);
    
    g_signal_connect(send_button, "clicked", G_CALLBACK(on_terminal_send_clicked), NULL);
    g_signal_connect(g_terminal_input_entry, "activate", G_CALLBACK(on_terminal_send_clicked), NULL);
    
    return vbox_main;
}

static void activate(GtkApplication *app, gpointer user_data) {
    g_app = app;
    
    g_main_window = gtk_application_window_new(g_app);
    gtk_window_set_title(GTK_WINDOW(g_main_window), "MegaEditor - Collaborative Edition");
    gtk_window_set_default_size(GTK_WINDOW(g_main_window), 300, 200);

    g_stack = gtk_stack_new();
    gtk_container_add(GTK_CONTAINER(g_main_window), g_stack);

    GtkWidget *login_view = create_login_view();
    gtk_stack_add_named(GTK_STACK(g_stack), login_view, "login_view");

    GtkWidget *editor_view = create_editor_view();
    gtk_stack_add_named(GTK_STACK(g_stack), editor_view, "editor_view");

    gtk_stack_set_visible_child_name(GTK_STACK(g_stack), "login_view");
    
    gtk_widget_show_all(g_main_window);
}

int main(int argc, char **argv) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    
    int status;
    GtkApplication *app = gtk_application_new("org.gtk.megathoneditor", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    
    curl_global_cleanup();
    return status;
}
