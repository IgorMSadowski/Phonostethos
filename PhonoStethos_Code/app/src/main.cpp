/**
 * main.cpp - starts the server executable.
 *
 * CONNECTIONS: The server creates SystemController from runtime options and runs th_Main; the GUI runner starts MyApplication, which embeds the Flutter view and executes lib/main.dart.
 *
 * FUNCTIONS:
 * - handle_signal(): request a service stop through the shared running flag.
 * - parse_options(): read command-line options and reject invalid values before startup.
 * - main(): start the program and hand execution to the application controller or GUI framework.
 * - runtime(): runtime.
 */



#include "phonostethos/SystemController.h" // Include the SystemController class/types used here; implements th_Main: validation, worker coordination, authoritative state and LED masks.

#include <csignal> // Include SIGINT/SIGTERM registration for service teardown.
#include <iostream> // Include console diagnostics and test results.

using namespace std; // Allow the names in this namespace without repeating its prefix.

namespace phono_app { // Keep these declarations in the project namespace and avoid global name collisions.
atomic<bool> running{true}; // Declare running for this operation.
} // End this block, declaration or argument list.

namespace { // Keep these declarations in the project namespace and avoid global name collisions.
/* API: signal
 * Documentation: https://man7.org/linux/man-pages/man2/signal.2.html
 * Register the SIGINT/SIGTERM service-stop callback. The callback only requests stop; ordinary code performs worker/resource teardown.
 */
void handle_signal(int) { phono_app::running.store(false); } // Publish the new shared void handle signal(int) { phono app::running value atomically.

bool parse_options(int argc, char** argv, phono_app::Options& options, // Pass this next argument/value to the call or initializer above.
                   bool& show_help) { // Declare show help for this operation.
    for (int i = 1; i < argc; ++i) { // Visit the specified items/indexes in order.
        const string arg = argv[i]; // Initialize arg to argv[i].
        if (arg == "--help") { // Test the condition arg == "--help".
            show_help = true; // Set show help to true.
            cout << "Usage: phonostethos-server [--bind IPv4] [--port PORT] " // Print the diagnostic or test result for the operator.
                         "[--capture-pcm NAME] [--playback-pcm NAME] " // Continue the expression or parameter list from the preceding line.
                         "[--storage PATH] [--led-device PATH] " // Continue the expression or parameter list from the preceding line.
                         "[--allow-unmounted-storage]\n"; // Complete this expression or declaration.
            return false; // Report failure or no available item to the caller.
        } // End this block, declaration or argument list.
        if (arg == "--allow-unmounted-storage") { // Test the condition arg == "--allow-unmounted-storage".
            options.require_mounted_storage = false; // Set options.require mounted storage to false.
            continue; // Skip the rest of this pass and check the next loop iteration.
        } // End this block, declaration or argument list.
        if (i + 1 >= argc) { // Test the condition i + 1 >= argc.
            cerr << "Missing value for " << arg << '\n'; // Print the diagnostic or test result for the operator.
            return false; // Report failure or no available item to the caller.
        } // End this block, declaration or argument list.
        const string value = argv[++i]; // Initialize value to argv[++i].
        if (arg == "--bind") { // Test the condition arg == "--bind".
            options.bind_address = value; // Set options.bind address to value.
        } else if (arg == "--capture-pcm") { // Follow the alternative path when the previous condition is false.
            options.capture_pcm = value; // Set options.capture pcm to value.
        } else if (arg == "--playback-pcm") { // Follow the alternative path when the previous condition is false.
            options.playback_pcm = value; // Set options.playback pcm to value.
        } else if (arg == "--storage") { // Follow the alternative path when the previous condition is false.
            options.storage = value; // Set options.storage to value.
        } else if (arg == "--led-device") { // Follow the alternative path when the previous condition is false.
            options.led_device = value; // Set options.led device to value.
        } else if (arg == "--port") { // Follow the alternative path when the previous condition is false.
            // Read decimal digits explicitly. Check the range after each digit
            // so even a very long argument cannot overflow the accumulator.
            unsigned port = 0; // Initialize port to 0.
            if (value.empty()) { // Test the condition value.empty().
                cerr << "Missing port number\n"; // Print the diagnostic or test result for the operator.
                return false; // Report failure or no available item to the caller.
            } // End this block, declaration or argument list.
            for (char digit : value) { // Visit the specified items/indexes in order.
                if (digit < '0' || digit > '9') { // Test the condition digit < '0' || digit > '9'.
                    cerr << "Invalid port: " << value << '\n'; // Print the diagnostic or test result for the operator.
                    return false; // Report failure or no available item to the caller.
                } // End this block, declaration or argument list.
                port = port * 10 + (digit - '0'); // Set port to port * 10 + (digit - '0').
                if (port > 65535) { // Test the condition port > 65535.
                    cerr << "Invalid port: " << value << '\n'; // Print the diagnostic or test result for the operator.
                    return false; // Report failure or no available item to the caller.
                } // End this block, declaration or argument list.
            } // End this block, declaration or argument list.
            if (port == 0) { // Test the condition port == 0.
                cerr << "Port must be between 1 and 65535\n"; // Print the diagnostic or test result for the operator.
                return false; // Report failure or no available item to the caller.
            } // End this block, declaration or argument list.
            options.port = static_cast<uint16_t>(port); // Set options.port to static_cast<uint16_t>(port).
        } else { // Follow the alternative path when the previous condition is false.
            cerr << "Unknown option: " << arg << '\n'; // Print the diagnostic or test result for the operator.
            return false; // Report failure or no available item to the caller.
        } // End this block, declaration or argument list.
    } // End this block, declaration or argument list.
    return true; // Report success, readiness or an available result to the caller.
} // End this block, declaration or argument list.
}  // namespace

int main(int argc, char** argv) { // Start the program and hand execution to the application controller or GUI framework.
    phono_app::Options options; // Declare options for this operation.
    bool show_help = false; // Initialize show help to false.
    if (!parse_options(argc, argv, options, show_help)) { // Test the condition !parse_options(argc, argv, options, show_help).
        if (show_help) { // Test the condition show_help.
            return 0; // Return 0 to the caller.
        } // End this block, declaration or argument list.
        return 2; // Return 2 to the caller.
    } // End this block, declaration or argument list.
    signal(SIGINT, handle_signal); // Call signal with the arguments shown here.
    signal(SIGTERM, handle_signal); // Call signal with the arguments shown here.

    phono_app::SystemController runtime(options); // Complete the call above with these remaining arguments.
    if (!runtime.initSystem()) { // Test the condition !runtime.initSystem().
        return 1; // Return 1 to the caller.
    } // End this block, declaration or argument list.
    runtime.run(); // Call runtime.run with the arguments shown here.
    return 0; // Return 0 to the caller.
} // End this block, declaration or argument list.
