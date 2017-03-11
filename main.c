/*
 * Copyright (c) 2017 William Pitcock
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <stdlib.h>
#include <unistd.h>

#include <gtk/gtk.h>
#include <vte/vte.h>

#define P_READ		0
#define P_WRITE		1

gchar *arg_status_str = "Processing a package manager transaction";
gchar *arg_title_str = "Package manager";

typedef struct {
	GtkWidget *mainwin;
	GtkWidget *mainwin_vbox;
	GtkWidget *status_str;
	GtkWidget *vte;
	GtkWidget *vte_expander;
	GtkWidget *progress_bar;
	GtkWidget *mainwin_bbox;
	GtkWidget *mainwin_close_btn;

	GIOChannel *progress_ioc;
	GIOChannel *stdout_ioc;
	GIOChannel *stderr_ioc;

	char **apk_argv;
	char scratch[32];

	int pipe_progress[2];
	int pipe_stdout[2];
	int pipe_stderr[2];

	GPid child_pid;
} Transaction;

GtkWidget *
mainwin_new(Transaction *t)
{
	char *markup;

	t->mainwin = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_title(GTK_WINDOW(t->mainwin), arg_title_str);
	gtk_container_set_border_width(GTK_CONTAINER(t->mainwin), 16);

	t->mainwin_vbox = gtk_vbox_new(FALSE, 8);
	gtk_container_add(GTK_CONTAINER(t->mainwin), t->mainwin_vbox);
	gtk_widget_show(t->mainwin_vbox);

	t->status_str = gtk_label_new(NULL);
	gtk_box_pack_start(GTK_BOX(t->mainwin_vbox), t->status_str, FALSE, FALSE, 0);
	markup = g_markup_printf_escaped("<big>%s</big>", arg_status_str);
	gtk_label_set_markup(GTK_LABEL(t->status_str), markup);
	gtk_misc_set_alignment(GTK_MISC(t->status_str), 0, 0); 
	g_free(markup);
	gtk_widget_show(t->status_str);

	t->progress_bar = gtk_progress_bar_new();
	gtk_box_pack_start(GTK_BOX(t->mainwin_vbox), t->progress_bar, FALSE, FALSE, 0);
	gtk_widget_show(t->progress_bar);

	t->vte_expander = gtk_expander_new("Details");
	gtk_expander_set_spacing(GTK_EXPANDER(t->vte_expander), 8);
	gtk_box_pack_start(GTK_BOX(t->mainwin_vbox), t->vte_expander, FALSE, FALSE, 0);
	gtk_widget_show(t->vte_expander);

	t->vte = vte_terminal_new();
	gtk_container_add(GTK_CONTAINER(t->vte_expander), t->vte);
	gtk_widget_show(t->vte);

	t->mainwin_bbox = gtk_hbutton_box_new();
	gtk_button_box_set_layout(GTK_BUTTON_BOX(t->mainwin_bbox), GTK_BUTTONBOX_END);
	gtk_box_pack_start(GTK_BOX(t->mainwin_vbox), t->mainwin_bbox, FALSE, FALSE, 0);
	gtk_widget_show(t->mainwin_bbox);

	t->mainwin_close_btn = gtk_button_new_from_stock(GTK_STOCK_CLOSE);
	gtk_box_pack_end(GTK_BOX(t->mainwin_bbox), t->mainwin_close_btn, FALSE, FALSE, 0);
	gtk_widget_set_sensitive(t->mainwin_close_btn, FALSE);
	gtk_widget_show(t->mainwin_close_btn);

	/* XXX: block closing the window when child process is still running */
	g_signal_connect(G_OBJECT(t->mainwin), "delete-event", G_CALLBACK(gtk_main_quit), NULL);
	g_signal_connect(G_OBJECT(t->mainwin_close_btn), "clicked", G_CALLBACK(gtk_main_quit), NULL);

	gtk_widget_show(t->mainwin);
	return t->mainwin;
}

gboolean
mainwin_transaction_progress_io_cb(GIOChannel *io, GIOCondition condition, gpointer data)
{
	Transaction *t = data;
	GError *error = NULL;
	gchar *line;
	gsize len;

	if (g_io_channel_read_line(io, &line, &len, NULL, &error) != G_IO_STATUS_NORMAL)
	{
		fprintf(stderr, "i/o error: %s\n", error->message);
		abort();
	}

	double done, total;
	if (sscanf(line, "%lf/%lf", &done, &total) == 2)
	{
		if (!total)
			goto out;

		double progress = done / total;
		gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(t->progress_bar), progress);
	}

out:
	g_free(line);

	return TRUE;
}

gboolean
mainwin_transaction_output_io_cb(GIOChannel *io, GIOCondition condition, gpointer data)
{
	Transaction *t = data;
	GError *error = NULL;
	gchar *line;
	gsize len;

	if (g_io_channel_read_line(io, &line, &len, NULL, &error) != G_IO_STATUS_NORMAL)
	{
		fprintf(stderr, "i/o error: %s\n", error->message);
		abort();
	}

	vte_terminal_feed(VTE_TERMINAL(t->vte), line, len);
	vte_terminal_feed(VTE_TERMINAL(t->vte), "\r", 1);
	g_free(line);

	return TRUE;
}

void
transaction_child_setup(gpointer data)
{
	int fd = *(int *) data;
	close(fd);
}

void
transaction_child_watch_cb(GPid pid, gint status, gpointer data)
{
	Transaction *t = data;

	gtk_widget_set_sensitive(t->mainwin_close_btn, TRUE);
	g_spawn_close_pid(pid);
}

Transaction *
transaction_new(GList *args)
{
	GError *error = NULL;
	Transaction *t = g_new0(Transaction, 1);
	gsize n = 0;

	if (pipe(t->pipe_progress) < 0)
	{
		abort();
		return NULL;
	}

	t->apk_argv = g_new0(char *, g_list_length(args) + 5);

	t->apk_argv[n++] = "apk";
	t->apk_argv[n++] = "--no-progress";

	snprintf(t->scratch, sizeof(t->scratch), "%d", t->pipe_progress[P_WRITE]);
	t->apk_argv[n++] = "--progress-fd";
	t->apk_argv[n++] = t->scratch;

	for (GList *l = args; l != NULL; l = l->next)
		t->apk_argv[n++] = l->data;

	t->apk_argv[n] = NULL;

#ifdef DEBUG
	fprintf(stderr, "command line: ");
	for (int i = 0; t->apk_argv[i]; i++)
		fprintf(stderr, "'%s' ", t->apk_argv[i]);
	fprintf(stderr, "\n");
#endif

	t->mainwin = mainwin_new(t);

	if (!g_spawn_async_with_pipes(NULL, t->apk_argv, NULL,
		G_SPAWN_LEAVE_DESCRIPTORS_OPEN | G_SPAWN_SEARCH_PATH | G_SPAWN_CHILD_INHERITS_STDIN | G_SPAWN_DO_NOT_REAP_CHILD,
		transaction_child_setup, &t->pipe_progress[P_READ],
		&t->child_pid, NULL,
		t->pipe_stdout, t->pipe_stderr, &error))
	{
		fprintf(stderr, "cant spawn: %s\n", error->message);
		abort();
	}
	g_child_watch_add(t->child_pid, transaction_child_watch_cb, t);

	t->progress_ioc = g_io_channel_unix_new(t->pipe_progress[P_READ]);
	g_io_add_watch(t->progress_ioc, G_IO_IN, mainwin_transaction_progress_io_cb, t);

	t->stdout_ioc = g_io_channel_unix_new(t->pipe_stdout[P_READ]);
	g_io_add_watch(t->stdout_ioc, G_IO_IN, mainwin_transaction_output_io_cb, t);

	t->stderr_ioc = g_io_channel_unix_new(t->pipe_stderr[P_READ]);
	g_io_add_watch(t->stderr_ioc, G_IO_IN, mainwin_transaction_output_io_cb, t);

	return t;
}

void
transaction_destroy(Transaction *t)
{
	g_free(t);
}

static const GOptionEntry gopt_entries[] = {
	{"status-str", 0, 0, G_OPTION_ARG_STRING, &arg_status_str, "status message to display while running a transaction", NULL},
	{"title-str", 0, 0, G_OPTION_ARG_STRING, &arg_title_str, "title string to use for the window", NULL},
	{NULL}
};

int
main(int argc, char *argv[])
{
	GError *error = NULL;
	GOptionContext *context;
	GList *args = NULL;
	Transaction *t = NULL;

	context = g_option_context_new(NULL);
	g_option_context_add_main_entries(context, gopt_entries, NULL);
	g_option_context_add_group(context, gtk_get_option_group(FALSE));
	g_option_context_set_ignore_unknown_options(context, TRUE);
	if (!g_option_context_parse(context, &argc, &argv, &error))
	{
		fprintf(stderr, "option parsing failed: %s\n", error->message);
		return EXIT_FAILURE;
	}

	gtk_init(&argc, &argv);

	if (argc < 2)
	{
		fprintf(stderr, "no command requested\n");
		return EXIT_FAILURE;
	}

	for (int i = 1; i < argc; i++)
		args = g_list_prepend(args, argv[i]);

	args = g_list_reverse(args);
	t = transaction_new(args);

	gtk_main();

	transaction_destroy(t);

	return EXIT_SUCCESS;
}
