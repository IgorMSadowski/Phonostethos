/**
 * main.cc - starts the Linux GTK/Flutter desktop runner.
 *
 * CONNECTIONS: The server creates SystemController from runtime options and runs th_Main; the GUI runner starts MyApplication, which embeds the Flutter view and executes lib/main.dart.
 *
 * FUNCTIONS:
 * - main(): start the program and hand execution to the application controller or GUI framework.
 */

#include "my_application.h" // Include the GTK application type implemented by my_application.cc.

int main(int argc, char** argv) { // Start the program and hand execution to the application controller or GUI framework.
  g_autoptr(MyApplication) app = my_application_new(); // Call g_autoptr with the arguments shown here.
  return g_application_run(G_APPLICATION(app), argc, argv); // Return g_application_run(G_APPLICATION(app), argc, argv) to the caller.
} // End this block, declaration or argument list.
