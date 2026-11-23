/**
 * my_application.h - hosts the Dart dashboard inside a Linux GTK application.
 *
 * CONNECTIONS: my_application.h declares MyApplication; my_application.cc implements window/lifecycle callbacks. linux/main.cc starts this runner. The application behavior and TCP client are in lib/main.dart, not in the GTK runner.
 *
 * FUNCTIONS:
 * - my_application_new(): provide the GTK/Flutter runner callback my_application_new.
 */

#ifndef FLUTTER_MY_APPLICATION_H_ // Compile this section only when the preprocessor condition holds.
#define FLUTTER_MY_APPLICATION_H_ // Define FLUTTER_MY_APPLICATION_H_ for the preprocessor.

#include <gtk/gtk.h> // Include GTK application/window declarations for the Linux Flutter runner.

G_DECLARE_FINAL_TYPE(MyApplication, my_application, MY, APPLICATION, // Declare/register the GTK application type using the framework macro.
                     GtkApplication) // Continue the expression or parameter list from the preceding line.

/**
 * my_application_new:
 *
 * Creates a new Flutter-based application.
 *
 * Returns: a new #MyApplication.
 */
MyApplication* my_application_new(); // Complete the call above with these remaining arguments.

#endif  // FLUTTER_MY_APPLICATION_H_
