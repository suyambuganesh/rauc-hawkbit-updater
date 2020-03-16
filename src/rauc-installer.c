/**
 * SPDX-License-Identifier: LGPL-2.1-only
 *
 * Copyright (C) 2018-2020 Prevas A/S (www.prevas.com)
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * @file rauc-installer.c
 * @author Lasse Mikkelsen <lkmi@prevas.dk>
 * @date 19 Sep 2018
 * @brief RAUC client
 *
 */

#include "rauc-installer.h"

/**
 * @brief RAUC DBUS property changed callback
 *
 * @see https://github.com/rauc/rauc/blob/master/src/rauc-installer.xml
 */
static void on_installer_status(GDBusProxy *proxy, GVariant *changed,
                                const gchar* const *invalidated,
                                gpointer data)
{
        struct install_context *context = data;
        gint32 percentage, depth;
        const gchar *message = NULL;

        if (invalidated && invalidated[0]) {
                g_printerr("RAUC DBUS service disappeared\n");
                g_mutex_lock(&context->status_mutex);
                context->status_result = 2;
                g_mutex_unlock(&context->status_mutex);
                g_main_loop_quit(context->mainloop);
                return;
        }

        if (context->notify_event) {
                g_mutex_lock(&context->status_mutex);
                if (g_variant_lookup(changed, "Operation", "s", &message)) {
                        g_queue_push_tail(&context->status_messages, g_strdup(message));
                } else if (g_variant_lookup(changed, "Progress", "(isi)", &percentage, &message, &depth)) {
                        g_queue_push_tail(&context->status_messages, g_strdup_printf("%3"G_GINT32_FORMAT "%% %s", percentage, message));
                } else if (g_variant_lookup(changed, "LastError", "s", &message) && message[0] != '\0') {
                        g_queue_push_tail(&context->status_messages, g_strdup_printf("LastError: %s", message));
                }
                g_mutex_unlock(&context->status_mutex);

                if (!g_queue_is_empty(&context->status_messages)) {
                        g_main_context_invoke(context->loop_context, context->notify_event, context);
                }
        }
}

/**
 * @brief RAUC DBUS complete signal callback
 *
 * @see https://github.com/rauc/rauc/blob/master/src/rauc-installer.xml
 */
static void on_installer_completed(GDBusProxy *proxy, gint result,
                                   gpointer data)
{
        struct install_context *context = data;

        g_mutex_lock(&context->status_mutex);
        context->status_result = result;
        g_mutex_unlock(&context->status_mutex);

        if (result >= 0) {
                g_main_loop_quit(context->mainloop);
        }
}

/**
 * @brief Create and init a install_context
 *
 * @return Pointer to initialized install_context struct. Should be freed by calling
 *         install_context_free().
 */
static struct install_context *install_context_new(void)
{
        struct install_context *context = g_new0(struct install_context, 1);

        g_mutex_init(&context->status_mutex);
        g_queue_init(&context->status_messages);
        context->status_result = -2;

        return context;
}

/**
 * @brief Free a install_context and its members
 *
 * @param[in] context the install_context struct that should be freed.
 *                    If NULL
 */
static void install_context_free(struct install_context *context)
{
        if (context == NULL)
                return;
        g_free(context->bundle);
        g_mutex_clear(&context->status_mutex);
        g_main_context_unref(context->loop_context);
        g_assert_cmpint(context->status_result, >=, 0);
        g_assert_true(g_queue_is_empty(&context->status_messages));
        g_main_loop_unref(context->mainloop);
        g_free(context);
}

/**
 * @brief RAUC client mainloop
 *
 * Install mainloop running until installation completes.
 * @param[in] data pointer to a install_context struct.
 * @return NULL is always returned.
 */
static gpointer install_loop_thread(gpointer data)
{
        GBusType bus_type = (!g_strcmp0(g_getenv("DBUS_STARTER_BUS_TYPE"), "session"))
                            ? G_BUS_TYPE_SESSION : G_BUS_TYPE_SYSTEM;
        RInstaller *r_installer_proxy = NULL;
        GError *error = NULL;
        struct install_context *context = data;
        g_main_context_push_thread_default(context->loop_context);

        g_debug("Creating RAUC DBUS proxy");
        r_installer_proxy = r_installer_proxy_new_for_bus_sync(bus_type,
                                                               G_DBUS_PROXY_FLAGS_GET_INVALIDATED_PROPERTIES,
                                                               "de.pengutronix.rauc", "/", NULL, &error);
        if (r_installer_proxy == NULL) {
                g_printerr("Error creating proxy: %s\n", error->message);
                g_clear_error(&error);
                goto out_loop;
        }
        if (g_signal_connect(r_installer_proxy, "g-properties-changed",
                             G_CALLBACK(on_installer_status), context) <= 0) {
                g_printerr("Failed to connect properties-changed signal\n");
                goto out_loop;
        }
        if (g_signal_connect(r_installer_proxy, "completed",
                             G_CALLBACK(on_installer_completed), context) <= 0) {
                g_printerr("Failed to connect completed signal\n");
                goto out_loop;
        }

        g_debug("Trying to contact RAUC DBUS service");
        if (!r_installer_call_install_sync(r_installer_proxy, context->bundle, NULL,
                                           &error)) {
                g_printerr("Failed %s\n", error->message);
                g_clear_error(&error);
                goto out_loop;
        }

        g_main_loop_run(context->mainloop);

out_loop:
        g_signal_handlers_disconnect_by_data(r_installer_proxy, context);

        // Notify the result of the RAUC installation
        if (context->notify_complete)
                context->notify_complete(context);

        g_main_context_pop_thread_default(context->loop_context);
        install_context_free(context);
        return NULL;
}

/**
 * @brief RAUC install bundle
 *
 * @param[in] bundle RAUC bundle file (.raucb) to install.
 * @param[in] on_install_notify Callback function to be called with status info during installation.
 * @param[in] on_install_complete Callback function to be called with the result of the installation.
 */
void rauc_install(const gchar *bundle, GSourceFunc on_install_notify, GSourceFunc on_install_complete)
{
        GMainContext *loop_context = g_main_context_new();
        struct install_context *context;

        context = install_context_new();
        context->bundle = g_strdup(bundle);
        context->notify_event = on_install_notify;
        context->notify_complete = on_install_complete;
        context->mainloop = g_main_loop_new(loop_context, FALSE);
        context->loop_context = loop_context;
        context->status_result = 2;

        g_thread_new("installer", install_loop_thread, (gpointer) context);
}
