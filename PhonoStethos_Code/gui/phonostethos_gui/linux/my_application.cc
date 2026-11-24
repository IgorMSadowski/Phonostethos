/**
 * my_application.cc - hosts the Dart dashboard inside a Linux GTK application.
 *
 * CONNECTIONS: my_application.h declares MyApplication; my_application.cc implements window/lifecycle callbacks. linux/main.cc starts this runner. The application behavior and TCP client are in lib/main.dart, not in the GTK runner.
 *
 * FUNCTIONS:
 * - my_application_activate(): provide the GTK/Flutter runner callback my_application_activate.
 * - my_application_local_command_line(): provide the GTK/Flutter runner callback my_application_local_command_line.
 * - my_application_startup(): provide the GTK/Flutter runner callback my_application_startup.
 * - my_application_shutdown(): provide the GTK/Flutter runner callback my_application_shutdown.
 * - my_application_dispose(): provide the GTK/Flutter runner callback my_application_dispose.
 * - my_application_class_init(): provide the GTK/Flutter runner callback my_application_class_init.
 * - my_application_init(): provide the GTK/Flutter runner callback my_application_init.
 * - my_application_new(): provide the GTK/Flutter runner callback my_application_new.
 */

// API DOCUMENTATION FOR THIS FILE:
// GTK 3 application/window APIs used by the Linux desktop runner:
// https://docs.gtk.org/gtk3/
// GObject type registration, ownership and lifecycle:
// https://docs.gtk.org/gobject/
// GApplication registration, activation and process lifecycle:
// https://docs.gtk.org/gio/class.Application.html
// Flutter Linux embedder: project/view creation and plugin registration:
// https://api.flutter.dev/linux-embedder/
// These links document external library/OS APIs; project class methods are
// implemented in the corresponding project headers and source files.


#include "my_application.h" // Include the GTK application type implemented by my_application.cc.

#include <flutter_linux/flutter_linux.h> // Include the Flutter Linux engine/view API hosted by GTK.
#ifdef GDK_WINDOWING_X11 // Compile this section only when the preprocessor condition holds.
#include <gdk/gdkx.h> // Include X11 window-manager checks in the Linux desktop runner.
#endif // End this conditional compilation section.

#include "flutter/generated_plugin_registrant.h" // Include the generated registration entry point for installed Flutter plugins.

struct _MyApplication { // Group the related fields in one plain data record.
  GtkApplication parent_instance; // Declare parent instance for this operation.
  char** dart_entrypoint_arguments; // Declare dart entrypoint arguments for this operation.
}; // End this block, declaration or argument list.

G_DEFINE_TYPE(MyApplication, my_application, GTK_TYPE_APPLICATION) // Declare/register the GTK application type using the framework macro.

// Implements GApplication::activate.
static void my_application_activate(GApplication* application) { // Finish the parameters/condition and enter the block below.
  MyApplication* self = MY_APPLICATION(application); // Initialize self to MY_APPLICATION(application).
  GtkWindow* window = // Start initializing this local value from the expression on the next line.
      GTK_WINDOW(gtk_application_window_new(GTK_APPLICATION(application))); // Call GTK_WINDOW with the arguments shown here.

  // Use a header bar when running in GNOME as this is the common style used
  // by applications and is the setup most users will be using (e.g. Ubuntu
  // desktop).
  // If running on X and not using GNOME then just use a traditional title bar
  // in case the window manager does more exotic layout, e.g. tiling.
  // If running on Wayland assume the header bar will work (may need changing
  // if future cases occur).
  gboolean use_header_bar = TRUE; // Initialize use header bar to TRUE.
#ifdef GDK_WINDOWING_X11 // Compile this section only when the preprocessor condition holds.
  GdkScreen* screen = gtk_window_get_screen(window); // Initialize screen to gtk_window_get_screen(window).
  if (GDK_IS_X11_SCREEN(screen)) { // Test the condition GDK_IS_X11_SCREEN(screen).
    const gchar* wm_name = gdk_x11_screen_get_window_manager_name(screen); // Initialize wm name to gdk_x11_screen_get_window_manager_name(screen).
    if (g_strcmp0(wm_name, "GNOME Shell") != 0) { // Test the condition g_strcmp0(wm_name, "GNOME Shell") != 0.
      use_header_bar = FALSE; // Set use header bar to FALSE.
    } // End this block, declaration or argument list.
  } // End this block, declaration or argument list.
#endif // End this conditional compilation section.
  if (use_header_bar) { // Test the condition use_header_bar.
    GtkHeaderBar* header_bar = GTK_HEADER_BAR(gtk_header_bar_new()); // Initialize header bar to GTK_HEADER_BAR(gtk_header_bar_new()).
    gtk_widget_show(GTK_WIDGET(header_bar)); // Call gtk_widget_show with the arguments shown here.
    gtk_header_bar_set_title(header_bar, "phonostethos"); // Call gtk_header_bar_set_title with the arguments shown here.
    gtk_header_bar_set_show_close_button(header_bar, TRUE); // Call gtk_header_bar_set_show_close_button with the arguments shown here.
    gtk_window_set_titlebar(window, GTK_WIDGET(header_bar)); // Call gtk_window_set_titlebar with the arguments shown here.
  } else { // Follow the alternative path when the previous condition is false.
    gtk_window_set_title(window, "phonostethos"); // Call gtk_window_set_title with the arguments shown here.
  } // End this block, declaration or argument list.

  gtk_window_set_default_size(window, 1280, 720); // Call gtk_window_set_default_size with the arguments shown here.
  gtk_widget_show(GTK_WIDGET(window)); // Call gtk_widget_show with the arguments shown here.

  g_autoptr(FlDartProject) project = fl_dart_project_new(); // Call g_autoptr with the arguments shown here.
  fl_dart_project_set_dart_entrypoint_arguments(project, self->dart_entrypoint_arguments); // Call fl_dart_project_set_dart_entrypoint_arguments with the arguments shown here.

  FlView* view = fl_view_new(project); // Initialize view to fl_view_new(project).
  gtk_widget_show(GTK_WIDGET(view)); // Call gtk_widget_show with the arguments shown here.
  gtk_container_add(GTK_CONTAINER(window), GTK_WIDGET(view)); // Call gtk_container_add with the arguments shown here.

  fl_register_plugins(FL_PLUGIN_REGISTRY(view)); // Call fl_register_plugins with the arguments shown here.

  gtk_widget_grab_focus(GTK_WIDGET(view)); // Call gtk_widget_grab_focus with the arguments shown here.
} // End this block, declaration or argument list.

// Implements GApplication::local_command_line.
static gboolean my_application_local_command_line(GApplication* application, gchar*** arguments, int* exit_status) { // Finish the parameters/condition and enter the block below.
  MyApplication* self = MY_APPLICATION(application); // Initialize self to MY_APPLICATION(application).
  // Strip out the first argument as it is the binary name.
  self->dart_entrypoint_arguments = g_strdupv(*arguments + 1); // Set self->dart entrypoint arguments to g_strdupv(*arguments + 1).

  g_autoptr(GError) error = nullptr; // Call g_autoptr with the arguments shown here.
  if (!g_application_register(application, nullptr, &error)) { // Test the condition !g_application_register(application, nullptr, &error).
     g_warning("Failed to register: %s", error->message); // Call g_warning with the arguments shown here.
     *exit_status = 1; // Complete this expression or declaration.
     return TRUE; // Return TRUE to the caller.
  } // End this block, declaration or argument list.

  g_application_activate(application); // Call g_application_activate with the arguments shown here.
  *exit_status = 0; // Complete this expression or declaration.

  return TRUE; // Return TRUE to the caller.
} // End this block, declaration or argument list.

// Implements GApplication::startup.
static void my_application_startup(GApplication* application) { // Finish the parameters/condition and enter the block below.
  //MyApplication* self = MY_APPLICATION(object);

  // Perform any actions required at application startup.

  G_APPLICATION_CLASS(my_application_parent_class)->startup(application); // Call G_APPLICATION_CLASS with the arguments shown here.
} // End this block, declaration or argument list.

// Implements GApplication::shutdown.
static void my_application_shutdown(GApplication* application) { // Finish the parameters/condition and enter the block below.
  //MyApplication* self = MY_APPLICATION(object);

  // Perform any actions required at application shutdown.
/* API: shutdown
 * Documentation: https://man7.org/linux/man-pages/man2/shutdown.2.html
 * Disable the selected directions of a connected socket to interrupt/finish transport work. The descriptor still needs close afterward.
 */

  G_APPLICATION_CLASS(my_application_parent_class)->shutdown(application); // Call G_APPLICATION_CLASS with the arguments shown here.
} // End this block, declaration or argument list.

// Implements GObject::dispose.
static void my_application_dispose(GObject* object) { // Finish the parameters/condition and enter the block below.
  MyApplication* self = MY_APPLICATION(object); // Initialize self to MY_APPLICATION(object).
  g_clear_pointer(&self->dart_entrypoint_arguments, g_strfreev); // Call g_clear_pointer with the arguments shown here.
  G_OBJECT_CLASS(my_application_parent_class)->dispose(object); // Call G_OBJECT_CLASS with the arguments shown here.
} // End this block, declaration or argument list.

static void my_application_class_init(MyApplicationClass* klass) { // Finish the parameters/condition and enter the block below.
  G_APPLICATION_CLASS(klass)->activate = my_application_activate; // Call G_APPLICATION_CLASS with the arguments shown here.
  G_APPLICATION_CLASS(klass)->local_command_line = my_application_local_command_line; // Call G_APPLICATION_CLASS with the arguments shown here.
  G_APPLICATION_CLASS(klass)->startup = my_application_startup; // Call G_APPLICATION_CLASS with the arguments shown here.
  G_APPLICATION_CLASS(klass)->shutdown = my_application_shutdown; // Call G_APPLICATION_CLASS with the arguments shown here.
  G_OBJECT_CLASS(klass)->dispose = my_application_dispose; // Call G_OBJECT_CLASS with the arguments shown here.
} // End this block, declaration or argument list.

static void my_application_init(MyApplication* self) {} // Continue the expression or parameter list from the preceding line.

MyApplication* my_application_new() { // Finish the parameters/condition and enter the block below.
  return MY_APPLICATION(g_object_new(my_application_get_type(), // Return MY_APPLICATION(g_object_new(my_application_get_type(), to the caller.
                                     "application-id", APPLICATION_ID, // Pass this next argument/value to the call or initializer above.
                                     "flags", G_APPLICATION_NON_UNIQUE, // Pass this next argument/value to the call or initializer above.
                                     nullptr)); // Complete the call above with these remaining arguments.
} // End this block, declaration or argument list.
