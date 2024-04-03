// SPDX-License-Identifier: Apache-2.0

#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <stdio.h>

static gint timer_delay_ms = 100;
static gchar *server_socket_path;
static gchar *client_socket_path;
static GMainLoop *loop;
static gboolean survive_systemd_kill_signal = FALSE;

#define READ_BUFFER_LEN 127

// Command line arguments
static GOptionEntry entries[] = {
	{ "timer_delay_ms", 'd', 0, G_OPTION_ARG_INT, &timer_delay_ms,
	  "Timer delay in milliseconds", NULL, },
	{ "server_socket_path", 's', 0, G_OPTION_ARG_FILENAME,
	  &server_socket_path, "Server UNIX domain socket path to listen on",
	  NULL, },
	{ "client_socket_path", 'c', 0, G_OPTION_ARG_FILENAME,
	  &client_socket_path, "UNIX domain socket path to read current state",
	  NULL, },
	{ "survive_systemd_kill_signal", 0, 0, G_OPTION_ARG_NONE,
	  &survive_systemd_kill_signal,
	  "Set argv[0][0] to '@' when running in initrd", NULL },
	{ NULL }
};

struct counter_data {
	int counter;
};

struct connection_info {
	GSocketConnection *connection;
	struct counter_data *cntr;
	GString *buf;
	gboolean terminate_at_end;
};

static gboolean timer_callback(gpointer data)
{
	struct counter_data *cntr = data;

	g_message("%d", cntr->counter++);

	return G_SOURCE_CONTINUE;
}

/*
 * The next block of functions are for the server that's exposed on a UNIX
 * domain socket. This is all done with asynchronous IO so that nothing will
 * block the glib main loop.
 */

void server_free_connection(struct connection_info *conn)
{
	g_object_unref(G_SOCKET_CONNECTION(conn->connection));
	g_string_free(conn->buf, TRUE);
	g_free(conn);
}

void server_message_sent(GObject *source_object, GAsyncResult *res,
			 gpointer user_data)
{
	GOutputStream *ostream = G_OUTPUT_STREAM(source_object);
	struct connection_info *conn = user_data;
	gboolean terminate = conn->terminate_at_end;
	g_autoptr(GError) error = NULL;
	gboolean success;

	success = g_output_stream_write_all_finish(ostream, res, NULL, &error);
	if (error != NULL) {
		g_printerr("%s", error->message);
		server_free_connection(conn);
		return;
	} else if (!success) {
		server_free_connection(conn);
		return;
	}

	if (terminate) {
		server_free_connection(conn);
		g_main_loop_quit(loop);
	}
}

void server_send_message(struct connection_info *conn)
{
	g_output_stream_write_all_async(g_io_stream_get_output_stream(G_IO_STREAM(conn->connection)),
					conn->buf->str, conn->buf->len,
					G_PRIORITY_DEFAULT, NULL,
					server_message_sent, conn);
}

#define SERVER_SET_COUNTER_COMMAND "set_counter "

void server_message_ready(GObject *source_object, GAsyncResult *res,
			  gpointer user_data)
{
	GInputStream *istream = G_INPUT_STREAM(source_object);
	struct connection_info *conn = user_data;
	g_autoptr(GError) error = NULL;
	int new_counter;
	gssize count;
	char *pos;

	count = g_input_stream_read_finish(istream, res, &error);
	if (error != NULL) {
		g_printerr("%s", error->message);
		server_free_connection(conn);
		return;
	} else if (count == 0) {
		server_free_connection(conn);
		return;
	}

	/*
	 * Note that this doesn't properly handle buffers containing
	 * multiple commands, or where a single command spans multiple
	 * buffers. This assumes one command per buffer since this
	 * program is just a proof of concept.
	 */
	conn->buf->str[conn->buf->allocated_len - 1] = '\0';
	if ((pos = strchr(conn->buf->str, '\n')) != NULL)
		*pos = '\0';
	conn->buf->len = strlen(conn->buf->str);

	if (g_str_equal(conn->buf->str, "get_counter")) {
		g_message("Returning counter to client");

		g_string_printf(conn->buf, "%d\n", conn->cntr->counter);
		server_send_message(conn);
	} else if (g_str_equal(conn->buf->str, "get_counter_and_terminate")) {
		g_message("Returning counter to client and terminating the process");

		conn->terminate_at_end = TRUE;
		g_string_printf(conn->buf, "%d\n", conn->cntr->counter);
		server_send_message(conn);
	} else if (g_str_has_prefix(conn->buf->str,
				    SERVER_SET_COUNTER_COMMAND)) {
		new_counter = g_ascii_strtoll(conn->buf->str + sizeof(SERVER_SET_COUNTER_COMMAND) - 1,
					      NULL, 10);

		g_message("Setting the counter to %d", new_counter);

		g_string_printf(conn->buf, "previous value %d\n",
				conn->cntr->counter);
		conn->cntr->counter = new_counter;
		server_send_message(conn);
	} else {
		g_message("Unknown message '%s' from client", conn->buf->str);

		g_string_printf(conn->buf, "Invalid command\n");
		server_send_message(conn);
	}

	g_string_set_size(conn->buf, READ_BUFFER_LEN);
	g_input_stream_read_async(istream, conn->buf->str,
				  conn->buf->allocated_len, G_PRIORITY_DEFAULT,
				  NULL, server_message_ready, conn);
}

static gboolean server_incoming_connection(GSocketService *service,
					   GSocketConnection *connection,
					   GObject *source_object,
					   gpointer user_data)
{
	struct connection_info *conn = g_new0(struct connection_info, 1);

	conn->connection = g_object_ref(connection);
	conn->cntr = user_data;
	conn->buf = g_string_sized_new(READ_BUFFER_LEN);

	g_input_stream_read_async(g_io_stream_get_input_stream(G_IO_STREAM(connection)),
				  conn->buf->str, conn->buf->allocated_len,
				  G_PRIORITY_DEFAULT, NULL,
				  server_message_ready, conn);

	return FALSE;
}

static GSocketService *create_unix_domain_server(char *server_socket_path,
						 struct counter_data *cntr)
{
	g_autoptr(GSocketService) service = NULL;
	g_autoptr(GSocketAddress) address = NULL;
	g_autoptr(GError) error = NULL;

	service = g_socket_service_new();
	if (service == NULL) {
		g_printerr("Error creating socket service.\n");
		return NULL;
	}

	address = g_unix_socket_address_new(server_socket_path);
	if (address == NULL) {
		g_printerr("Error creating socket address.\n");
		return NULL;
	}

	if (!g_socket_listener_add_address(G_SOCKET_LISTENER(service),
					   G_SOCKET_ADDRESS(address),
					   G_SOCKET_TYPE_STREAM,
					   G_SOCKET_PROTOCOL_DEFAULT,
					   NULL, NULL, &error)) {
		g_printerr("Error binding socket: %s\n", error->message);
		return NULL;
	}

	g_signal_connect(service, "incoming",
			 G_CALLBACK(server_incoming_connection), cntr);

	g_socket_service_start(service);

	return g_steal_pointer(&service);
}

/*
 * This is the client that reads the current state from another process
 * via a UNIX domain socket. This is done using synchronous IO since this
 * is only called on boot up and we will be blocked waiting to read the
 * current state.
 */

#define CLIENT_GET_COUNTER_COMMAND "get_counter_and_terminate\n"

int read_counter_from_server(gchar *server_path)
{
	g_autoptr(GSocketConnection) connection = NULL;
	g_autoptr(GSocketAddress) address = NULL;
	g_autoptr(GSocketClient) client = NULL;
	g_autoptr(GError) error = NULL;
	gssize bytes_read;
	gchar buf[100];

	client = g_socket_client_new();
	address = g_unix_socket_address_new(server_path);

	connection = g_socket_client_connect(client,
					     G_SOCKET_CONNECTABLE(address),
					     NULL, &error);
	if (error != NULL) {
		/*
		 * We shouldn't terminate when we can't read the current state.
		 * Just start over.
		 */
		g_printerr("Error connecting to socket: %s", error->message);
		return 0;
	}

	GInputStream *input_stream = g_io_stream_get_input_stream(G_IO_STREAM(connection));
	GOutputStream *output_stream = g_io_stream_get_output_stream(G_IO_STREAM(connection));

	g_output_stream_write(output_stream, CLIENT_GET_COUNTER_COMMAND,
			      strlen(CLIENT_GET_COUNTER_COMMAND), NULL, &error);
	if (error != NULL) {
		g_printerr("Error writing to socket: %s", error->message);
		return 0;
	}

	bytes_read = g_input_stream_read(input_stream, buf, sizeof(buf) - 1,
					 NULL, &error);
	if (error != NULL) {
		g_printerr("Error reading from socket: %s", error->message);
		return 0;
	}

	buf[bytes_read] = '\0';

	return g_ascii_strtoll(buf, NULL, 10);
}

int get_initial_counter(void)
{
	if (client_socket_path == NULL)
		return 0;

	g_message("Reading starting position from socket %s",
		  client_socket_path);

	return read_counter_from_server(client_socket_path);
}

int main(int argc, char **argv)
{
	g_autoptr(GSocketService) service = NULL;
	g_autoptr(GOptionContext) context = NULL;
	g_autoptr(GError) error = NULL;

	context = g_option_context_new("- Example Early Service");
	g_option_context_add_main_entries(context, entries, NULL);
	if (!g_option_context_parse(context, &argc, &argv, &error)) {
		g_printerr("option parsing failed: %s\n", error->message);
		return 1;
	}

	if (survive_systemd_kill_signal) {
		/*
		 * See https://systemd.io/ROOT_STORAGE_DAEMONS/ for more details
		 * about having this started inside the initrd running when the
		 * system transitions to running services from the root
		 * filesystem. systemd v255 and higher has the option
		 * SurviveFinalKillSignal=yes that can be used instead.
		 */
		argv[0][0] = '@';
	}

	loop = g_main_loop_new(NULL, FALSE);

	struct counter_data cntr = {
		.counter = get_initial_counter()
	};

	guint timer_id = g_timeout_add(timer_delay_ms, timer_callback, &cntr);

	if (server_socket_path != NULL) {
		g_message("Listening on UNIX socket %s", server_socket_path);
		service = create_unix_domain_server(server_socket_path, &cntr);
		if (service == NULL)
			return 1;
	} else
		g_message("Not listening on a UNIX socket.");

	g_main_loop_run(loop);

	g_source_remove(timer_id);
	if (service != NULL) {
		g_socket_service_stop(service);
		g_socket_listener_close(G_SOCKET_LISTENER(service));
		g_unlink(server_socket_path);
	}

	g_main_loop_unref(loop);

	return 0;
}
