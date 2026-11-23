/*
 * main.dart - implements the Flutter dashboard TCP client and its waveform/FFT/archive controls.
 *
 * CONNECTIONS: protocol.dart owns byte encoding/decoding. main.dart owns widget state and socket callbacks. The Pi remains the server and the authority for transport/mode/gain state. A submitted command stays pending until a final CommandResult arrives.
 *
 * FUNCTIONS:
 * - main(): start the program and hand execution to the application controller or GUI framework.
 * - build(): construct Flutter widgets from the current acknowledged state.
 * - createState(): create the mutable state object for the dashboard widget.
 * - _gainLabel(): display a fractional gain code as its real multiplier.
 * - initState(): initialize the dashboard state, start its clock and connect once.
 * - _connect(): open the GUI TCP connection, register receive callbacks and request the archive.
 * - Duration(): Duration.
 * - _receive(): feed received TCP chunks into the incremental frame decoder.
 * - _handleMessage(): apply a decoded server message to GUI state.
 * - _updateArchive(): merge one archive page by stable file ID and retain the next cursor.
 * - _handleCommandResult(): keep pending commands pending until a final server result arrives.
 * - _updatePlots(): prepare waveform, smoothed FFT power and decaying peak traces for display.
 * - _request(): mark one GUI command pending, encode five bytes and await local socket submission; server completion still arrives later.
 * - _refreshArchive(): clear the displayed archive and request its first page.
 * - _connectionEnded(): clear connection-specific GUI state without stopping Pi audio or recording.
 * - _scheduleReconnect(): show the connection problem and schedule another asynchronous connection attempt.
 * - dispose(): cancel timers/subscriptions and close the GUI socket when the widget is removed.
 * - Column(): Column.
 * - _statusChip(): draw a compact connection/storage status label.
 * - _buildListeningTab(): construct the Listening Tab portion of the dashboard.
 * - _chartPanel(): chart Panel.
 * - _buildControls(): construct the Controls portion of the dashboard.
 * - Text(): Text.
 * - _toggleRecording(): request START_REC or STOP_REC according to the acknowledged transport.
 * - _sendMode(): request the selected GENERAL/HEART/LUNG filter profile.
 * - _sendGain(): send the selected slider code after validating GUI readiness.
 * - _buildModeButton(): construct the Mode Button portion of the dashboard.
 * - _modeName(): return the display name for the selected filter code.
 * - _modeBand(): return the plotted frequency range for the selected mode.
 * - RangeValues(): Range Values.
 * - _bandDescription(): display the selected band-pass cutoff range.
 * - _buildArchiveTab(): construct the Archive Tab portion of the dashboard.
 * - Icon(): Icon.
 * - _confirmDelete(): ask the user to confirm the selected file before submitting its deletion.
 * - Padding(): Padding.
 */

import 'dart:math' show sqrt; // Import square-root/logarithm functions used to display spectrum amplitudes.
import 'dart:async'; // Import timers, stream subscriptions and asynchronous cleanup helpers.
import 'dart:io'; // Import the Dart TCP Socket client and SocketException.

import 'package:fl_chart/fl_chart.dart'; // Import waveform and FFT chart widgets and FlSpot points.
import 'package:flutter/material.dart'; // Import Flutter widgets, state lifecycle and the Material dashboard controls.

import 'protocol.dart'; // Import the matching command encoder, frame decoder and message records.

void main() => runApp(const PhonoStethosApp()); // Start the program and hand execution to the application controller or GUI framework.

class PhonoStethosApp extends StatelessWidget { // Declare PhonoStethosApp and its related operations/state.
  const PhonoStethosApp({super.key}); // Complete the call above with these remaining arguments.

  @override // Implement the callback defined by the Flutter base class.
  Widget build(BuildContext context) => MaterialApp( // Begin this function/widget call; its arguments follow below.
        title: 'PhonoStethos Dashboard', // Set the window, panel or control title.
        debugShowCheckedModeBanner: false, // Set the debug Show Checked Mode Banner property of this widget/record.
        theme: ThemeData.dark().copyWith( // Set the theme property of this widget/record.
          primaryColor: Colors.teal, // Set the primary Color property of this widget/record.
          scaffoldBackgroundColor: const Color(0xff141414), // Set the scaffold Background Color property of this widget/record.
          colorScheme: ThemeData.dark().colorScheme.copyWith( // Set the color Scheme property of this widget/record.
                primary: Colors.tealAccent, // Set the primary property of this widget/record.
                secondary: Colors.orangeAccent, // Set the secondary property of this widget/record.
              ), // End this block, declaration or argument list.
        ), // End this block, declaration or argument list.
        home: const DashboardScreen(), // Set the home property of this widget/record.
      ); // End this block, declaration or argument list.
} // End this block, declaration or argument list.

class DashboardScreen extends StatefulWidget { // Declare DashboardScreen and its related operations/state.
  const DashboardScreen({super.key}); // Complete the call above with these remaining arguments.

  @override // Implement the callback defined by the Flutter base class.
  State<DashboardScreen> createState() => _DashboardScreenState(); // Create the mutable state object for the dashboard widget.
} // End this block, declaration or argument list.

class _DashboardScreenState extends State<DashboardScreen> { // Declare _DashboardScreenState and its related operations/state.
  static const _host = '10.42.0.3'; // Initialize host to '10.42.0.3'.
  static const _port = 8080; // Initialize port to 8080.

  final _decoder = FrameDecoder(); // Incremental parser that retains incomplete TCP frame bytes between callbacks.
  final _recordings = <FileEntry>[]; // The displayed archive of server-issued file IDs and basenames.
  Socket? _socket; // Current GUI TCP client socket; null means there is no connected socket.
  StreamSubscription<List<int>>? _subscription; // The receive/error/closure callbacks registered on the current socket.
  Timer? _reconnectTimer; // One delayed reconnect attempt; null prevents assuming a retry is scheduled.
  bool _connecting = false; // Initialize connecting to false.
  bool _connected = false; // Initialize connected to false.
  bool _storageMounted = false; // Initialize storage Mounted to false.
  bool _allowRecord = false; // Initialize allow Record to false.
  bool _allowPlayback = false; // Initialize allow Playback to false.
  bool _archiveLoading = false; // Initialize archive Loading to false.
  String? _errorText; // Declare error Text for this operation.
  Command? _pendingCommand; // Command awaiting the Pi's final result; null means the GUI may submit another.
  int? _archiveCursor; // The next server-provided archive page cursor, or null when no page remains.
  int _mode = 0; // Initialize mode to 0.
  static const _gainCodes = [ // Initialize gain Codes to [.
    gainHalf, // Name this field, choice or argument: gain Half.
    gainThreeQuarters, // Name this field, choice or argument: gain Three Quarters.
    gainFourFifths, // Name this field, choice or argument: gain Four Fifths.
    1, // Pass this next argument/value to the call or initializer above.
    2, // Pass this next argument/value to the call or initializer above.
    3, // Pass this next argument/value to the call or initializer above.
    4, // Pass this next argument/value to the call or initializer above.
    5, // Pass this next argument/value to the call or initializer above.
    6, // Pass this next argument/value to the call or initializer above.
    7, // Pass this next argument/value to the call or initializer above.
    8, // Pass this next argument/value to the call or initializer above.
    9, // Pass this next argument/value to the call or initializer above.
    10, // Pass this next argument/value to the call or initializer above.
    11, // Pass this next argument/value to the call or initializer above.
    12, // Pass this next argument/value to the call or initializer above.
    13, // Pass this next argument/value to the call or initializer above.
    14, // Pass this next argument/value to the call or initializer above.
    15, // Pass this next argument/value to the call or initializer above.
    16, // Pass this next argument/value to the call or initializer above.
    17, // Pass this next argument/value to the call or initializer above.
    18, // Pass this next argument/value to the call or initializer above.
    19, // Pass this next argument/value to the call or initializer above.
    20, // Pass this next argument/value to the call or initializer above.
  ]; // End this block, declaration or argument list.
  int _gain = 1; // Initialize gain to 1.
  int? _gainPreview; // Temporary slider position; it does not claim the Pi has acknowledged a new gain.
  // Wire codes 101, 102 and 103 represent the fractional gain choices.
  String _gainLabel(int code) { // Display a fractional gain code as its real multiplier.
    if (code == gainHalf) return '0.5×'; // Test the condition code == gainHalf. If true, return '0.5×' to the caller.
    if (code == gainThreeQuarters) return '0.75×'; // Test the condition code == gainThreeQuarters. If true, return '0.75×' to the caller.
    if (code == gainFourFifths) return '0.8×'; // Test the condition code == gainFourFifths. If true, return '0.8×' to the caller.
    return '$code×'; // Return '$code×' to the caller.
  } // End this block, declaration or argument list.

  Transport _transport = Transport.live; // Initialize transport to Transport.live.
  List<FlSpot> _waveform = const [FlSpot(0, 0)]; // Initialize waveform to const [FlSpot(0, 0)].
  List<double> _spectrumPower = []; // Smoothed linear POWER values; convert their square roots to dBFS for plotting.
  List<FlSpot> _spectrumPeaks = []; // Peak trace points with an 8 dB/s decay, independent of the smoothed live trace.
  final _spectrumClock = Stopwatch(); // Initialize spectrum Clock to Stopwatch().
  int _lastSpectrumUs = 0; // Initialize last Spectrum Us to 0.
  List<FlSpot> _spectrum = const [FlSpot(0, -120)]; // Initialize spectrum to const [FlSpot(0, -120)].

  @override // Implement the callback defined by the Flutter base class.
  void initState() { // Initialize the dashboard state, start its clock and connect once.
    super.initState(); // Call super.initState with the arguments shown here.
    _spectrumClock.start(); // Call _spectrumClock.start with the arguments shown here.
    _connect(); // Open the GUI TCP connection, register receive callbacks and request the archive.
  } // End this block, declaration or argument list.

  // FLOWCHART: GUI events -> connect to Raspberry Pi -> receive server packets.
  Future<void> _connect() async { // Open the GUI TCP connection, register receive callbacks and request the archive.
    if (_connecting || _connected || !mounted) return; // Test the condition _connecting || _connected || !mounted. If true, leave this function; any local scoped locks are released.
    _connecting = true; // Set connecting to true.
    try { // Catch failures from the operations in this block.
      // Dart API: Socket.connect(host, port, timeout: ...).
      // Opens TCP asynchronously. await waits for the connection without freezing the UI.
      // The Pi must be listening at this address. SocketException/TimeoutException handle failure.
      final socket = await Socket.connect( // Initialize socket to await Socket.connect(.
        _host, // Name this field, choice or argument: host.
        _port, // Name this field, choice or argument: port.
        timeout: const Duration(seconds: 3), // Set the timeout property of this widget/record.
      ); // End this block, declaration or argument list.
      if (!mounted) { // Test the condition !mounted.
        socket.destroy(); // Call socket.destroy with the arguments shown here.
        return; // Leave this function; any local scoped locks are released.
      } // End this block, declaration or argument list.
      _socket = socket; // Set socket to socket.
      _connected = true; // Set connected to true.
      _decoder.clear(); // Discard the previous contents/state of this object.
      _recordings.clear(); // Discard the previous contents/state of this object.
      _archiveCursor = 0; // Set archive Cursor to 0.
      _pendingCommand = null; // Set pending Command to null.
      _errorText = null; // Set error Text to null.
      // Flutter API: setState(callback).
      // Change the widget state in the callback. Flutter then rebuilds the screen.
      // The callback must be synchronous. Network reads happen outside it.
      setState(() {}); // Call setState with the arguments shown here.
      // Dart API: socket.listen(onData, onError: ..., onDone: ...).
      // Registers callbacks for received bytes, failure and connection closure.
      // The returned subscription is cancelled when this dashboard is disposed.
      _subscription = socket.listen( // Set subscription to socket.listen(.
        _receive, // Name this field, choice or argument: receive.
        onError: (Object error, StackTrace stack) => // Handle a TCP stream failure.
            _connectionEnded('TCP connection failed: $error'), // Call _connectionEnded with the arguments shown here.
        onDone: () => _connectionEnded('Disconnected from the Raspberry Pi'), // Handle the TCP stream closing.
        cancelOnError: true, // Stop the subscription if the stream reports an error.
      ); // End this block, declaration or argument list.
      _request(Command.getArchive, 0); // Mark one GUI command pending, encode five bytes and submit them to the socket.
    } on SocketException catch (error) { // Handle the stated exception instead of continuing with an invalid result.
      _scheduleReconnect('Cannot connect to $_host: $_port: ${error.message}'); // Show the connection problem and schedule another asynchronous connection attempt.
    } on TimeoutException { // Continue the expression or parameter list from the preceding line.
      _scheduleReconnect('Connection to the Raspberry Pi timed out'); // Show the connection problem and schedule another asynchronous connection attempt.
    } finally { // Continue the expression or parameter list from the preceding line.
      _connecting = false; // Set connecting to false.
    } // End this block, declaration or argument list.
  } // End this block, declaration or argument list.

  void _receive(List<int> bytes) { // Feed received TCP chunks into the incremental frame decoder.
    try { // Catch failures from the operations in this block.
      for (final message in _decoder.add(bytes)) { // Visit the specified items/indexes in order.
        _handleMessage(message); // Apply a decoded server message to GUI state.
      } // End this block, declaration or argument list.
    } on FormatException catch (error) { // Handle the stated exception instead of continuing with an invalid result.
      _connectionEnded('Invalid server response: ${error.message}'); // Clear connection-specific GUI state without stopping Pi audio or recording.
    } // End this block, declaration or argument list.
  } // End this block, declaration or argument list.

  void _handleMessage(ServerMessage message) { // Apply a decoded server message to GUI state.
    if (!mounted) return; // Test the condition !mounted. If true, leave this function; any local scoped locks are released.
    setState(() { // Call setState with the arguments shown here.
      if (message is StatusMessage) { // Test the condition message is StatusMessage.
        _storageMounted = message.storageMounted; // Set storage Mounted to message.storageMounted.
        _allowRecord = message.allowRecord; // Set allow Record to message.allowRecord.
        _allowPlayback = message.allowPlayback; // Set allow Playback to message.allowPlayback.
        if (message.errorCode != 0) { // Test the condition message.errorCode != 0.
          _errorText = 'Server error ${message.errorCode}'; // Set error Text to 'Server error ${message.errorCode}'.
        } // End this block, declaration or argument list.

        return; // Leave this function; any local scoped locks are released.
      } // End this block, declaration or argument list.
      if (message is StateMessage) { // Test the condition message is StateMessage.
        if (_transport != message.transport || // Test the condition _transport != message.transport ||.
            _mode != message.filter || // Continue this calculation/condition with the next line.
            _gain != message.gain) { // Finish the parameters/condition and enter the block below.
          _spectrumPower = []; // Set spectrum Power to [].
          _spectrumPeaks = []; // Set spectrum Peaks to [].
        } // End this block, declaration or argument list.
        _transport = message.transport; // Set transport to message.transport.
        _mode = message.filter; // Set mode to message.filter.
        _gain = message.gain; // Set gain to message.gain.
        if (message.errorCode != 0) { // Test the condition message.errorCode != 0.
          _errorText = 'Server error ${message.errorCode}'; // Set error Text to 'Server error ${message.errorCode}'.
        } // End this block, declaration or argument list.

        return; // Leave this function; any local scoped locks are released.
      } // End this block, declaration or argument list.
      if (message is DataMessage) { // Test the condition message is DataMessage.
        _updatePlots(message); // Prepare waveform, smoothed FFT power and decaying peak traces for display.

        return; // Leave this function; any local scoped locks are released.
      } // End this block, declaration or argument list.
      if (message is FileListMessage) { // Test the condition message is FileListMessage.
        _updateArchive(message); // Merge one archive page by stable file ID and retain the next cursor.
        return; // Leave this function; any local scoped locks are released.
      } // End this block, declaration or argument list.
      if (message is CommandResultMessage) { // Test the condition message is CommandResultMessage.
        _handleCommandResult(message); // Keep pending commands pending until a final server result arrives.
        return; // Leave this function; any local scoped locks are released.
      } // End this block, declaration or argument list.
    }); // End this block, declaration or argument list.
  } // End this block, declaration or argument list.

  // Called inside setState: add one archive page without duplicating file IDs.
  void _updateArchive(FileListMessage message) { // Merge one archive page by stable file ID and retain the next cursor.
    for (final file in message.entries) { // Visit the specified items/indexes in order.
      bool alreadyListed = false; // Initialize already Listed to false.
      for (final listedFile in _recordings) { // Visit the specified items/indexes in order.
        if (listedFile.id == file.id) { // Test the condition listedFile.id == file.id.
          alreadyListed = true; // Set already Listed to true.
          break; // Leave the current loop or switch branch.
        } // End this block, declaration or argument list.
      } // End this block, declaration or argument list.
      if (!alreadyListed) _recordings.add(file); // Test the condition !alreadyListed. If true, call _recordings.add with the arguments shown here.
    } // End this block, declaration or argument list.
    _archiveCursor = null; // Set archive Cursor to null.
    if (message.nextCursor != 0) { // Test the condition message.nextCursor != 0.
      _archiveCursor = message.nextCursor; // Set archive Cursor to message.nextCursor.
    } // End this block, declaration or argument list.
  } // End this block, declaration or argument list.

  // FLOWCHART: server reply -> clear pending -> refresh controls/archive.
  // AcceptedPending is only an acknowledgement; keep waiting for the final result.
  void _handleCommandResult(CommandResultMessage message) { // Keep pending commands pending until a final server result arrives.
    if (_pendingCommand != message.command) { // Test the condition _pendingCommand != message.command.
      _errorText = 'Unexpected command response'; // Set error Text to 'Unexpected command response'.
      return; // Leave this function; any local scoped locks are released.
    } // End this block, declaration or argument list.
    if (message.outcome == CommandOutcome.acceptedPending) { // Test the condition message.outcome == CommandOutcome.acceptedPending.
      return; // Leave this function; any local scoped locks are released.
    } // End this block, declaration or argument list.
    _pendingCommand = null; // Set pending Command to null.
    _archiveLoading = false; // Set archive Loading to false.
    if (message.outcome == CommandOutcome.rejected || // Test the condition message.outcome == CommandOutcome.rejected ||.
        message.outcome == CommandOutcome.failed) { // Continue checking the file/state comparison; no value is assigned here.
      _errorText = // Continue the expression or parameter list from the preceding line.
          'Command ${message.command.name} failed (code ${message.detail})'; // Complete this expression or declaration.
      return; // Leave this function; any local scoped locks are released.
    } // End this block, declaration or argument list.
    _errorText = null; // Set error Text to null.
    if (message.command == Command.getArchive && _archiveCursor != null) { // Test the condition message.command == Command.getArchive && _archiveCursor != null.
      final cursor = _archiveCursor!; // Initialize cursor to _archiveCursor!.
      _archiveCursor = null; // Set archive Cursor to null.
      WidgetsBinding.instance.addPostFrameCallback((_) { // Finish the parameters/condition and enter the block below.
        if (mounted) _request(Command.getArchive, cursor); // Test the condition mounted. If true, mark one gui command pending, encode five bytes and submit them to the socket.
      }); // End this block, declaration or argument list.
    } else if (message.command == Command.deleteRecording) { // Follow the alternative path when the previous condition is false.
      _refreshArchive(); // Clear the displayed archive and request its first page.
    } // End this block, declaration or argument list.
  } // End this block, declaration or argument list.

  // Called inside setState after a complete Data frame has been decoded.
  // The Pi computes FFT values. These loops only prepare points for the charts.
  void _updatePlots(DataMessage data) { // Prepare waveform, smoothed FFT power and decaying peak traces for display.
    _waveform = []; // Set waveform to [].
    for (int i = 0; i < data.waveform.length; i++) { // Visit the specified items/indexes in order.
      _waveform.add(FlSpot(i.toDouble(), data.waveform[i])); // Call _waveform.add with the arguments shown here.
    } // End this block, declaration or argument list.

    bool firstFrame = _spectrumPower.length != data.spectrum.length; // Initialize first Frame to _spectrumPower.length != data.spectrum.length.
    if (firstFrame) _spectrumPower = List.filled(data.spectrum.length, 0.0); // Test the condition firstFrame. If true, set spectrum power to list.filled(data.spectrum.length, 0.0).
    for (int i = 0; i < data.spectrum.length; i++) { // Visit the specified items/indexes in order.
      double power = data.spectrum[i] * data.spectrum[i]; // Initialize power to data.spectrum[i] * data.spectrum[i].
      if (firstFrame) { // Test the condition firstFrame.
        _spectrumPower[i] = power; // Set spectrum Power[i] to power.
      } else { // Follow the alternative path when the previous condition is false.
        _spectrumPower[i] = 0.35 * _spectrumPower[i] + 0.65 * power; // Set spectrum Power[i] to 0.35 * _spectrumPower[i] + 0.65 * power.
      } // End this block, declaration or argument list.
    } // End this block, declaration or argument list.

    int now = _spectrumClock.elapsedMicroseconds; // Initialize now to _spectrumClock.elapsedMicroseconds.
    double decayDb = (now - _lastSpectrumUs) * 8 / 1000000; // Initialize decay Db to (now - _lastSpectrumUs) * 8 / 1000000.
    _lastSpectrumUs = now; // Set last Spectrum Us to now.
    int visibleBins = (_modeBand(_mode).end * 1024 / 8000).floor() + 1; // Initialize visible Bins to (_modeBand(_mode).end * 1024 / 8000).floor() + 1.
    final previousPeaks = _spectrumPeaks; // Initialize previous Peaks to _spectrumPeaks.
    _spectrum = []; // Set spectrum to [].
    _spectrumPeaks = []; // Set spectrum Peaks to [].
    for (int i = 0; i < visibleBins; i++) { // Visit the specified items/indexes in order.
      double frequency = i * 8000 / 1024; // Initialize frequency to i * 8000 / 1024.
      double peakDb = spectrumDbfs(data.spectrum[i]); // Initialize peak Db to spectrumDbfs(data.spectrum[i]).
      if (i < previousPeaks.length) { // Test the condition i < previousPeaks.length.
        double previousDb = previousPeaks[i].y - decayDb; // Initialize previous Db to previousPeaks[i].y - decayDb.
        if (previousDb > peakDb) peakDb = previousDb; // Test the condition previousDb > peakDb. If true, set peak db to previousdb.
      } // End this block, declaration or argument list.
      _spectrumPeaks.add(FlSpot(frequency, peakDb)); // Call _spectrumPeaks.add with the arguments shown here.
      double liveDb = spectrumDbfs(sqrt(_spectrumPower[i])); // Initialize live Db to spectrumDbfs(sqrt(_spectrumPower[i])).
      _spectrum.add(FlSpot(frequency, liveDb)); // Call _spectrum.add with the arguments shown here.
    } // End this block, declaration or argument list.
  } // End this block, declaration or argument list.

  // The GUI sends a request. The Pi validates it and returns CommandResult/State.
  // One pending command prevents conflicting button presses while workers finish.
  // FLOWCHART: GUI send command -> mark pending -> send five bytes.
  Future<void> _request(Command command, int value) async { // Mark one GUI command pending, encode five bytes and submit them to the socket.
    if (!_connected || _pendingCommand != null) return; // Test the condition !_connected || _pendingCommand != null. If true, leave this function; any local scoped locks are released.
    final socket = _socket; // Initialize socket to _socket.
    if (socket == null) return; // Test the condition socket == null. If true, leave this function; any local scoped locks are released.
    try { // Catch failures from the operations in this block.
      _pendingCommand = command; // Set pending Command to command.
      if (command == Command.getArchive) { // Test the condition command == Command.getArchive.
        _archiveLoading = true; // Set archive Loading to true.
      } // End this block, declaration or argument list.
      // Dart API: Socket.add(bytes) queues bytes for TCP transmission.
      // Socket.flush() requests delivery of buffered bytes and returns a Future.
      // The server CommandResult, rather than this call, confirms the requested operation.
      if (mounted) setState(() {}); // Test the condition mounted. If true, call setstate with the arguments shown here.
      socket.add(encodeCommand(command, value)); // Call socket.add with the arguments shown here.
      await socket.flush(); // Wait for local socket submission so asynchronous send errors reach this try/catch.
    } on SocketException catch (error) { // Handle the stated exception instead of continuing with an invalid result.
      _connectionEnded('Failed to send command: ${error.message}'); // Clear connection-specific GUI state without stopping Pi audio or recording.
    } on StateError catch (error) { // A socket closed between checking it and submitting bytes.
      _connectionEnded('Cannot submit command: $error'); // Clear the pending request and use the ordinary reconnect path.
    } // End this block, declaration or argument list.
  } // End this block, declaration or argument list.

  void _refreshArchive() { // Clear the displayed archive and request its first page.
    _recordings.clear(); // Discard the previous contents/state of this object.
    _archiveCursor = null; // Set archive Cursor to null.
    _request(Command.getArchive, 0); // Mark one GUI command pending, encode five bytes and submit them to the socket.
  } // End this block, declaration or argument list.

  // This disconnects the dashboard only; it does not stop the Pi's recording.
  void _connectionEnded(String reason) { // Clear connection-specific GUI state without stopping Pi audio or recording.
    if (!_connected && _reconnectTimer != null) return; // Test the condition !_connected && _reconnectTimer != null. If true, leave this function; any local scoped locks are released.
    _connected = false; // Set connected to false.
    _pendingCommand = null; // Set pending Command to null.
    _archiveLoading = false; // Set archive Loading to false.
    _socket?.destroy(); // Complete the call above with these remaining arguments.
    _socket = null; // Set socket to null.
    unawaited(_subscription?.cancel()); // Call unawaited with the arguments shown here.
    _subscription = null; // Set subscription to null.
    _scheduleReconnect(reason); // Show the connection problem and schedule another asynchronous connection attempt.
  } // End this block, declaration or argument list.

  void _scheduleReconnect(String reason) { // Show the connection problem and schedule another asynchronous connection attempt.
    if (!mounted) return; // Test the condition !mounted. If true, leave this function; any local scoped locks are released.
    setState(() { // Call setState with the arguments shown here.
      _errorText = reason; // Set error Text to reason.
    }); // End this block, declaration or argument list.
    if (_reconnectTimer != null) return; // A retry is already scheduled; do not create a second timer.
    _reconnectTimer = Timer(const Duration(seconds: 3), () { // Set reconnect Timer to Timer(const Duration(seconds: 3), () {.
      _reconnectTimer = null; // Set reconnect Timer to null.
      _connect(); // Open the GUI TCP connection, register receive callbacks and request the archive.
    }); // End this block, declaration or argument list.
  } // End this block, declaration or argument list.

  @override // Implement the callback defined by the Flutter base class.
  void dispose() { // Cancel timers/subscriptions and close the GUI socket when the widget is removed.
    _reconnectTimer?.cancel(); // Complete the call above with these remaining arguments.
    unawaited(_subscription?.cancel()); // Call unawaited with the arguments shown here.
    _socket?.destroy(); // Complete the call above with these remaining arguments.
    super.dispose(); // Call super.dispose with the arguments shown here.
  } // End this block, declaration or argument list.

  @override // Implement the callback defined by the Flutter base class.
  Widget build(BuildContext context) { // Construct Flutter widgets from the current acknowledged state.
    return DefaultTabController( // Return DefaultTabController( to the caller.
      length: 2, // Set the length property of this widget/record.
      child: Scaffold( // Place this widget inside its parent.
        appBar: AppBar( // Set the app Bar property of this widget/record.
          title: const Text('PhonoStethos Live Clinical Dashboard'), // Set the window, panel or control title.
          backgroundColor: Colors.black45, // Choose this element's background color.
          actions: [ // Set the actions property of this widget/record.
            _statusChip( // Call _statusChip with the arguments shown here.
              _connected ? 'USB-C Link Active' : 'USB-C Disconnected', // Pass this next argument/value to the call or initializer above.
              _connected ? Colors.green.shade800 : Colors.red.shade800, // Pass this next argument/value to the call or initializer above.
            ), // End this block, declaration or argument list.
            const SizedBox(width: 8), // Pass this next argument/value to the call or initializer above.
            _statusChip( // Call _statusChip with the arguments shown here.
              _storageMounted ? 'Storage Mounted' : 'No Storage', // Pass this next argument/value to the call or initializer above.
              _storageMounted ? Colors.blue.shade800 : Colors.grey.shade700, // Pass this next argument/value to the call or initializer above.
            ), // End this block, declaration or argument list.
            const SizedBox(width: 16), // Pass this next argument/value to the call or initializer above.
          ], // End this block, declaration or argument list.
          bottom: TabBar( // Set the bottom property of this widget/record.
            indicatorColor: Colors.tealAccent, // Set the indicator Color property of this widget/record.
            labelColor: Colors.tealAccent, // Set the label Color property of this widget/record.
            unselectedLabelColor: Colors.white60, // Set the unselected Label Color property of this widget/record.
            tabs: [ // Set the tabs property of this widget/record.
              const Tab( // Begin this function/widget call; its arguments follow below.
                icon: Icon(Icons.show_chart), // Set the icon property of this widget/record.
                text: 'Live Auscultation & Controls', // Display this label.
              ), // End this block, declaration or argument list.
              Tab( // Call Tab with the arguments shown here.
                icon: const Icon(Icons.folder_special), // Set the icon property of this widget/record.
                text: 'Recordings Archive (${_recordings.length})', // Display this label.
              ), // End this block, declaration or argument list.
            ], // End this block, declaration or argument list.
            onTap: (index) { // Set the on Tap property of this widget/record.
              if (index == 1) _refreshArchive(); // Test the condition index == 1. If true, clear the displayed archive and request its first page.
            }, // End this block, declaration or argument list.
          ), // End this block, declaration or argument list.
        ), // End this block, declaration or argument list.
        body: Column( // Set the body property of this widget/record.
          children: [ // Place these child widgets in the order listed.
            if (_errorText != null) // Test the condition _errorText != null.
              MaterialBanner( // Call MaterialBanner with the arguments shown here.
                content: Text(_errorText!), // Set the content property of this widget/record.
                leading: const Icon(Icons.warning_amber_rounded), // Set the leading property of this widget/record.
                actions: [ // Set the actions property of this widget/record.
                  TextButton( // Call TextButton with the arguments shown here.
                    onPressed: () => setState(() => _errorText = null), // Enable this button with the callback below; null disables it.
                    child: const Text('DISMISS'), // Place this widget inside its parent.
                  ), // End this block, declaration or argument list.
                ], // End this block, declaration or argument list.
              ), // End this block, declaration or argument list.
            Expanded( // Call Expanded with the arguments shown here.
              child: TabBarView( // Place this widget inside its parent.
                children: [_buildListeningTab(), _buildArchiveTab()], // Place these child widgets in the order listed.
              ), // End this block, declaration or argument list.
            ), // End this block, declaration or argument list.
          ], // End this block, declaration or argument list.
        ), // End this block, declaration or argument list.
      ), // End this block, declaration or argument list.
    ); // End this block, declaration or argument list.
  } // End this block, declaration or argument list.

  Widget _statusChip(String label, Color color) => Padding( // Begin this function/widget call; its arguments follow below.
        padding: const EdgeInsets.symmetric(vertical: 8), // Leave this spacing around the child widget.
        child: Chip( // Place this widget inside its parent.
          visualDensity: VisualDensity.compact, // Set the visual Density property of this widget/record.
          label: Text(label), // Set the label property of this widget/record.
          side: BorderSide(color: color.withValues(alpha: 0.6)), // Set the side property of this widget/record.
          backgroundColor: color.withValues(alpha: 0.12), // Choose this element's background color.
        ), // End this block, declaration or argument list.
      ); // End this block, declaration or argument list.

  Widget _buildListeningTab() => LayoutBuilder( // Begin this function/widget call; its arguments follow below.
        builder: (context, constraints) { // Build this widget when the framework requests it.
          final narrow = constraints.maxWidth < 850; // Initialize narrow to constraints.maxWidth < 850.
          final charts = Column( // Initialize charts to Column(.
            children: [ // Place these child widgets in the order listed.
              Expanded( // Call Expanded with the arguments shown here.
                child: _chartPanel( // Place this widget inside its parent.
                  'Processed waveform', // Pass this next argument/value to the call or initializer above.
                  '128 ms history · normalized PCM16', // Pass this next argument/value to the call or initializer above.
                  _waveform, // Name this field, choice or argument: waveform.
                  Colors.tealAccent, // Pass this next argument/value to the call or initializer above.
                  minY: -1, // Set the min Y property of this widget/record.
                  maxY: 1, // Set the max Y property of this widget/record.
                  maxX: 1024, // Set the max X property of this widget/record.
                ), // End this block, declaration or argument list.
              ), // End this block, declaration or argument list.
              const SizedBox(height: 12), // Pass this next argument/value to the call or initializer above.
              Expanded( // Call Expanded with the arguments shown here.
                child: _chartPanel( // Place this widget inside its parent.
                  'Frequency spectrum · ${_modeName(_mode)}', // Pass this next argument/value to the call or initializer above.
                  '${_bandDescription(_mode)} · orange: live · cyan: recent peaks', // Pass this next argument/value to the call or initializer above.
                  _spectrum, // Name this field, choice or argument: spectrum.
                  Colors.orangeAccent, // Pass this next argument/value to the call or initializer above.
                  minY: -90, // Set the min Y property of this widget/record.
                  maxY: 0, // Set the max Y property of this widget/record.
                  maxX: _modeBand(_mode).end, // Set the max X property of this widget/record.
                  peakSpots: _spectrumPeaks, // Peak trace points with an 8 dB/s decay, independent of the smoothed live trace.
                  highlightedRange: _modeBand(_mode), // Set the highlighted Range property of this widget/record.
                  frequencyAxis: true, // Set the frequency Axis property of this widget/record.
                ), // End this block, declaration or argument list.
              ), // End this block, declaration or argument list.
            ], // End this block, declaration or argument list.
          ); // End this block, declaration or argument list.
          final controls = _buildControls(); // Initialize controls to _buildControls().
          return Padding( // Return Padding( to the caller.
            padding: const EdgeInsets.all(16), // Leave this spacing around the child widget.
            child: narrow // Place this widget inside its parent.
                ? ListView( // Begin this function/widget call; its arguments follow below.
                    children: [ // Place these child widgets in the order listed.
                      SizedBox(height: 560, child: charts), // Call SizedBox with the arguments shown here.
                      const SizedBox(height: 16), // Pass this next argument/value to the call or initializer above.
                      controls, // Name this field, choice or argument: controls.
                    ], // End this block, declaration or argument list.
                  ) // End this block, declaration or argument list.
                : Row( // Supply the next constructor initializer or argument.
                    children: [ // Place these child widgets in the order listed.
                      Expanded(flex: 3, child: charts), // Call Expanded with the arguments shown here.
                      const SizedBox(width: 16), // Pass this next argument/value to the call or initializer above.
                      SizedBox(width: 340, child: controls), // Call SizedBox with the arguments shown here.
                    ], // End this block, declaration or argument list.
                  ), // End this block, declaration or argument list.
          ); // End this block, declaration or argument list.
        }, // End this block, declaration or argument list.
      ); // End this block, declaration or argument list.

  Widget _buildControls() => Card( // Begin this function/widget call; its arguments follow below.
        child: ListView( // Place this widget inside its parent.
          padding: const EdgeInsets.all(20), // Leave this spacing around the child widget.
          children: [ // Place these child widgets in the order listed.
            Text('Transport: ${_transport.name.toUpperCase()}'), // Call Text with the arguments shown here.
            const SizedBox(height: 16), // Pass this next argument/value to the call or initializer above.
            const Text( // Begin this function/widget call; its arguments follow below.
              'DSP Filter Mode', // Pass this next argument/value to the call or initializer above.
              style: TextStyle(fontSize: 16, fontWeight: FontWeight.bold), // Set the style property of this widget/record.
            ), // End this block, declaration or argument list.
            const SizedBox(height: 12), // Pass this next argument/value to the call or initializer above.
            _buildModeButton(0, 'General (20 - 2000 Hz)', Colors.blue), // Call _buildModeButton with the arguments shown here.
            _buildModeButton(1, 'Heart Murmurs (20 - 600 Hz)', Colors.red), // Call _buildModeButton with the arguments shown here.
            _buildModeButton(2, 'Lung Sounds (100 - 1000 Hz)', Colors.amber), // Call _buildModeButton with the arguments shown here.
            const SizedBox(height: 8), // Pass this next argument/value to the call or initializer above.
            const Divider(), // Pass this next argument/value to the call or initializer above.
            Row( // Call Row with the arguments shown here.
              children: [ // Place these child widgets in the order listed.
                const Expanded(child: Text('Gain (live and playback)')), // Pass this next argument/value to the call or initializer above.
                Text(_gainLabel(_gainPreview ?? _gain)), // Call Text with the arguments shown here.
              ], // End this block, declaration or argument list.
            ), // End this block, declaration or argument list.
            Slider( // Call Slider with the arguments shown here.
              value: _gainCodes // Display the current acknowledged or preview value.
                  .indexOf(_gainPreview ?? _gain) // Continue the expression or parameter list from the preceding line.
                  .clamp(0, _gainCodes.length - 1) // Continue the expression or parameter list from the preceding line.
                  .toDouble(), // Pass this next argument/value to the call or initializer above.
              max: (_gainCodes.length - 1).toDouble(), // Set the maximum displayed control/chart value.
              divisions: _gainCodes.length - 1, // Choose the number of selectable slider intervals.
              label: _gainLabel(_gainPreview ?? _gain), // Set the label property of this widget/record.
              onChanged: _enabled // Handle a user changing this control.
                  ? (value) => // Continue the expression or parameter list from the preceding line.
                      setState(() => _gainPreview = _gainCodes[value.round()]) // Call setState with the arguments shown here.
                  : null, // Declare null for this operation.
              onChangeEnd: _enabled // Send the selected value after the user releases the slider.
                  ? (value) { // Finish the parameters/condition and enter the block below.
                      setState(() => _gainPreview = null); // Call setState with the arguments shown here.
                      _sendGain(_gainCodes[value.round()]); // Call _sendGain with the arguments shown here.
                    } // End this block, declaration or argument list.
                  : null, // Declare null for this operation.
            ), // End this block, declaration or argument list.
            const Text('Live Audio Capture'), // Pass this next argument/value to the call or initializer above.
            const SizedBox(height: 12), // Pass this next argument/value to the call or initializer above.
            FilledButton.icon( // Call FilledButton.icon with the arguments shown here.
              onPressed: _canToggleRecording ? _toggleRecording : null, // Enable this button with the callback below; null disables it.
              icon: Icon( // Set the icon property of this widget/record.
                _transport == Transport.recording // Continue checking the file/state comparison; no value is assigned here.
                    ? Icons.stop_circle_outlined // Continue the expression or parameter list from the preceding line.
                    : Icons.fiber_manual_record, // Supply the next constructor initializer or argument.
              ), // End this block, declaration or argument list.
              label: Text( // Set the label property of this widget/record.
                _transport == Transport.recording // Continue checking the file/state comparison; no value is assigned here.
                    ? 'Stop Recording' // Continue the expression or parameter list from the preceding line.
                    : 'Record Auscultation', // Supply the next constructor initializer or argument.
              ), // End this block, declaration or argument list.
            ), // End this block, declaration or argument list.
            const SizedBox(height: 8), // Pass this next argument/value to the call or initializer above.
            OutlinedButton.icon( // Call OutlinedButton.icon with the arguments shown here.
              onPressed: _enabled && _transport == Transport.playback // Enable this button with the callback below; null disables it.
                  ? () => _request(Command.stopPlayback, 0) // Continue the expression or parameter list from the preceding line.
                  : null, // Declare null for this operation.
              icon: const Icon(Icons.stop), // Set the icon property of this widget/record.
              label: const Text('Stop playback'), // Set the label property of this widget/record.
            ), // End this block, declaration or argument list.
            const SizedBox(height: 8), // Pass this next argument/value to the call or initializer above.
            OutlinedButton.icon( // Call OutlinedButton.icon with the arguments shown here.
              onPressed: _enabled && _storageMounted // Enable this button with the callback below; null disables it.
                  ? () => _request(Command.ejectStorage, 0) // Continue the expression or parameter list from the preceding line.
                  : null, // Declare null for this operation.
              icon: const Icon(Icons.eject), // Set the icon property of this widget/record.
              label: const Text('Eject USB Safely'), // Set the label property of this widget/record.
            ), // End this block, declaration or argument list.
          ], // End this block, declaration or argument list.
        ), // End this block, declaration or argument list.
      ); // End this block, declaration or argument list.

  bool get _enabled => _connected && _pendingCommand == null; // Complete this expression or declaration.

  // FLOWCHART: availability check. Stopping an active recording is allowed;
  // starting a new one also requires storage permission and live transport.
  bool get _canToggleRecording { // Continue the expression or parameter list from the preceding line.
    if (!_enabled) return false; // Test the condition !_enabled. If true, report failure or no available item to the caller.
    if (_transport == Transport.recording) return true; // Test the condition _transport == Transport.recording. If true, report success, readiness or an available result to the caller.
    return _allowRecord && _transport == Transport.live; // Return _allowRecord && _transport == Transport.live to the caller.
  } // End this block, declaration or argument list.

  void _toggleRecording() { // Finish the parameters/condition and enter the block below.
    if (_transport == Transport.recording) { // Test the condition _transport == Transport.recording.
      _request(Command.stopRecording, 0); // Mark one GUI command pending, encode five bytes and submit them to the socket.
    } else { // Follow the alternative path when the previous condition is false.
      _request(Command.startRecording, 0); // Mark one GUI command pending, encode five bytes and submit them to the socket.
    } // End this block, declaration or argument list.
  } // End this block, declaration or argument list.

  void _sendMode(int value) { // Finish the parameters/condition and enter the block below.
    setState(() => _mode = value); // Call setState with the arguments shown here.
    _request(Command.setMode, value); // Mark one GUI command pending, encode five bytes and submit them to the socket.
  } // End this block, declaration or argument list.

  void _sendGain(int value) { // Finish the parameters/condition and enter the block below.
    _request(Command.setGain, value); // Mark one GUI command pending, encode five bytes and submit them to the socket.
  } // End this block, declaration or argument list.

  Widget _buildModeButton(int mode, String label, Color activeColor) => Padding( // Begin this function/widget call; its arguments follow below.
        padding: const EdgeInsets.only(bottom: 8), // Leave this spacing around the child widget.
        child: ElevatedButton( // Place this widget inside its parent.
          style: ElevatedButton.styleFrom( // Set the style property of this widget/record.
            backgroundColor: _mode == mode ? activeColor : Colors.grey.shade900, // Choose this element's background color.
            foregroundColor: Colors.white, // Set the foreground Color property of this widget/record.
            minimumSize: const Size.fromHeight(42), // Set the minimum Size property of this widget/record.
          ), // End this block, declaration or argument list.
          onPressed: _enabled ? () => _sendMode(mode) : null, // Enable this button with the callback below; null disables it.
          child: Text(label, style: const TextStyle(fontSize: 14)), // Place this widget inside its parent.
        ), // End this block, declaration or argument list.
      ); // End this block, declaration or argument list.

  String _modeName(int mode) => // Continue the expression or parameter list from the preceding line.
      const ['General', 'Heart', 'Lung'][mode.clamp(0, 2)]; // Complete this expression or declaration.

  RangeValues _modeBand(int mode) { // Finish the parameters/condition and enter the block below.
    switch (mode) { // Choose one branch from the supplied command/state value.
      case 1: // Handle the 1 choice in this switch.
        return const RangeValues(20, 600); // Return const RangeValues(20, 600) to the caller.
      case 2: // Handle the 2 choice in this switch.
        return const RangeValues(100, 1000); // Return const RangeValues(100, 1000) to the caller.
      default: // Handle values not matched by an earlier case.
        return const RangeValues(20, 2000); // Return const RangeValues(20, 2000) to the caller.
    } // End this block, declaration or argument list.
  } // End this block, declaration or argument list.

  String _bandDescription(int mode) { // Finish the parameters/condition and enter the block below.
    final band = _modeBand(mode); // Initialize band to _modeBand(mode).
    return '${band.start.round()}–${band.end.round()} Hz passband'; // Return '${band.start.round()}–${band.end.round()} Hz passband' to the caller.
  } // End this block, declaration or argument list.

  Widget _buildArchiveTab() => Padding( // Begin this function/widget call; its arguments follow below.
        padding: const EdgeInsets.all(16), // Leave this spacing around the child widget.
        child: Column( // Place this widget inside its parent.
          crossAxisAlignment: CrossAxisAlignment.stretch, // Set the cross Axis Alignment property of this widget/record.
          children: [ // Place these child widgets in the order listed.
            Row( // Call Row with the arguments shown here.
              children: [ // Place these child widgets in the order listed.
                Text( // Call Text with the arguments shown here.
                  'USB recordings', // Pass this next argument/value to the call or initializer above.
                  style: Theme.of(context).textTheme.titleLarge, // Set the style property of this widget/record.
                ), // End this block, declaration or argument list.
                const Spacer(), // Pass this next argument/value to the call or initializer above.
                if (_archiveLoading) // Test the condition _archiveLoading.
                  const SizedBox.square( // Begin this function/widget call; its arguments follow below.
                    dimension: 22, // Set the dimension property of this widget/record.
                    child: CircularProgressIndicator(strokeWidth: 2), // Place this widget inside its parent.
                  ), // End this block, declaration or argument list.
                IconButton( // Call IconButton with the arguments shown here.
                  tooltip: 'Refresh archive', // Set the tooltip property of this widget/record.
                  onPressed: _enabled ? _refreshArchive : null, // Enable this button with the callback below; null disables it.
                  icon: const Icon(Icons.refresh), // Set the icon property of this widget/record.
                ), // End this block, declaration or argument list.
              ], // End this block, declaration or argument list.
            ), // End this block, declaration or argument list.
            const SizedBox(height: 8), // Pass this next argument/value to the call or initializer above.
            Expanded( // Call Expanded with the arguments shown here.
              child: _recordings.isEmpty // Place this widget inside its parent.
                  ? const Center(child: Text('No recordings in the archive')) // Continue the expression or parameter list from the preceding line.
                  : ListView.separated( // Supply the next constructor initializer or argument.
                      itemCount: _recordings.length, // Set the item Count property of this widget/record.
                      separatorBuilder: (_, __) => const Divider(height: 1), // Set the separator Builder property of this widget/record.
                      itemBuilder: (context, index) { // Set the item Builder property of this widget/record.
                        final file = _recordings[index]; // Initialize file to _recordings[index].
                        return ListTile( // Return ListTile( to the caller.
                          leading: const Icon(Icons.audio_file_outlined), // Set the leading property of this widget/record.
                          title: Text(file.basename), // Set the window, panel or control title.
                          subtitle: Text('File ID ${file.id}'), // Set the subtitle property of this widget/record.
                          trailing: Wrap( // Set the trailing property of this widget/record.
                            spacing: 4, // Set the spacing property of this widget/record.
                            children: [ // Place these child widgets in the order listed.
                              IconButton( // Call IconButton with the arguments shown here.
                                tooltip: 'Play recording', // Set the tooltip property of this widget/record.
                                onPressed: _enabled && // Enable this button with the callback below; null disables it.
                                        _allowPlayback && // Continue this calculation/condition with the next line.
                                        _transport == Transport.live // Continue checking the file/state comparison; no value is assigned here.
                                    ? () { // Finish the parameters/condition and enter the block below.
                                        _request(Command.playAudio, file.id); // Mark one GUI command pending, encode five bytes and submit them to the socket.
                                      } // End this block, declaration or argument list.
                                    : null, // Declare null for this operation.
                                icon: const Icon(Icons.play_arrow), // Set the icon property of this widget/record.
                              ), // End this block, declaration or argument list.
                              IconButton( // Call IconButton with the arguments shown here.
                                tooltip: 'Delete recording', // Set the tooltip property of this widget/record.
                                onPressed: _enabled // Enable this button with the callback below; null disables it.
                                    ? () => _confirmDelete(file) // Continue the expression or parameter list from the preceding line.
                                    : null, // Declare null for this operation.
                                icon: const Icon(Icons.delete_outline), // Set the icon property of this widget/record.
                              ), // End this block, declaration or argument list.
                            ], // End this block, declaration or argument list.
                          ), // End this block, declaration or argument list.
                        ); // End this block, declaration or argument list.
                      }, // End this block, declaration or argument list.
                    ), // End this block, declaration or argument list.
            ), // End this block, declaration or argument list.
          ], // End this block, declaration or argument list.
        ), // End this block, declaration or argument list.
      ); // End this block, declaration or argument list.

  Future<void> _confirmDelete(FileEntry file) async { // Continue the expression or parameter list from the preceding line.
    // Flutter API: showDialog<bool>(context: ..., builder: ...).
    // Shows a modal confirmation and returns the value supplied to Navigator.pop.
    // Only true allows the selected delete request to be sent.
    final confirmed = await showDialog<bool>( // Initialize confirmed to await showDialog<bool>(.
      context: context, // Set the context property of this widget/record.
      builder: (context) => AlertDialog( // Build this widget when the framework requests it.
        title: const Text('Delete recording?'), // Set the window, panel or control title.
        content: Text('Permanently delete ${file.basename}?'), // Set the content property of this widget/record.
        actions: [ // Set the actions property of this widget/record.
          TextButton( // Call TextButton with the arguments shown here.
            onPressed: () => Navigator.pop(context, false), // Enable this button with the callback below; null disables it.
            child: const Text('Cancel'), // Place this widget inside its parent.
          ), // End this block, declaration or argument list.
          FilledButton( // Call FilledButton with the arguments shown here.
            onPressed: () => Navigator.pop(context, true), // Enable this button with the callback below; null disables it.
            child: const Text('Delete'), // Place this widget inside its parent.
          ), // End this block, declaration or argument list.
        ], // End this block, declaration or argument list.
      ), // End this block, declaration or argument list.
    ); // End this block, declaration or argument list.
    if (confirmed == true && mounted) { // Test the condition confirmed == true && mounted.
      _request(Command.deleteRecording, file.id); // Mark one GUI command pending, encode five bytes and submit them to the socket.
    } // End this block, declaration or argument list.
  } // End this block, declaration or argument list.

  Widget _chartPanel( // Begin this function/widget call; its arguments follow below.
    String title, // Declare title for this operation.
    String subtitle, // Declare subtitle for this operation.
    List<FlSpot> spots, // Declare spots for this operation.
    Color color, { // Declare color for this operation.
    required double minY, // Pass this next argument/value to the call or initializer above.
    required double maxY, // Pass this next argument/value to the call or initializer above.
    required double maxX, // Pass this next argument/value to the call or initializer above.
    RangeValues? highlightedRange, // Declare highlighted Range for this operation.
    bool frequencyAxis = false, // Initialize frequency Axis to false.
    List<FlSpot> peakSpots = const [], // Initialize peak Spots to const [].
  }) => // Continue the expression or parameter list from the preceding line.
      Card( // Call Card with the arguments shown here.
        child: Padding( // Place this widget inside its parent.
          padding: const EdgeInsets.fromLTRB(16, 12, 16, 8), // Leave this spacing around the child widget.
          child: Column( // Place this widget inside its parent.
            crossAxisAlignment: CrossAxisAlignment.start, // Set the cross Axis Alignment property of this widget/record.
            children: [ // Place these child widgets in the order listed.
              Text(title, style: Theme.of(context).textTheme.titleMedium), // Call Text with the arguments shown here.
              Text(subtitle, style: Theme.of(context).textTheme.bodySmall), // Call Text with the arguments shown here.
              const SizedBox(height: 8), // Pass this next argument/value to the call or initializer above.
              Expanded( // Call Expanded with the arguments shown here.
// fl_chart API: LineChart(LineChartData(...)).
// Draws the series of FlSpot(frequencyHz, dBFS) points with explicit axis bounds.
// The chart consumes values computed above and does not perform the FFT.
                child: LineChart( // Place this widget inside its parent.
                  LineChartData( // Call LineChartData with the arguments shown here.
                    minX: 0, // Set the min X property of this widget/record.
                    maxX: maxX, // Set the max X property of this widget/record.
                    minY: minY, // Set the min Y property of this widget/record.
                    maxY: maxY, // Set the max Y property of this widget/record.
                    gridData: // Set the grid Data property of this widget/record.
                        const FlGridData(show: true, drawVerticalLine: false), // Pass this next argument/value to the call or initializer above.
                    borderData: FlBorderData(show: false), // Set the border Data property of this widget/record.
                    titlesData: const FlTitlesData( // Set the titles Data property of this widget/record.
                      topTitles: AxisTitles( // Set the top Titles property of this widget/record.
                        sideTitles: SideTitles(showTitles: false), // Set the side Titles property of this widget/record.
                      ), // End this block, declaration or argument list.
                      rightTitles: AxisTitles( // Set the right Titles property of this widget/record.
                        sideTitles: SideTitles(showTitles: false), // Set the side Titles property of this widget/record.
                      ), // End this block, declaration or argument list.
                    ).copyWith( // Begin this function/widget call; its arguments follow below.
                      bottomTitles: AxisTitles( // Set the bottom Titles property of this widget/record.
                        axisNameWidget: // Set the axis Name Widget property of this widget/record.
                            frequencyAxis ? const Text('Frequency (Hz)') : null, // Pass this next argument/value to the call or initializer above.
                        sideTitles: SideTitles( // Set the side Titles property of this widget/record.
                          showTitles: frequencyAxis, // Set the show Titles property of this widget/record.
                          interval: maxX <= 600 ? 100 : maxX / 4, // Set the interval property of this widget/record.
                          reservedSize: 28, // Set the reserved Size property of this widget/record.
                          getTitlesWidget: (value, meta) => SideTitleWidget( // Set the get Titles Widget property of this widget/record.
                            meta: meta, // Set the meta property of this widget/record.
                            child: Text( // Place this widget inside its parent.
                              value.round().toString(), // Call value.round with the arguments shown here.
                              style: const TextStyle( // Set the style property of this widget/record.
                                color: Colors.white70, // Choose this element's display color.
                                fontSize: 9, // Set the font Size property of this widget/record.
                              ), // End this block, declaration or argument list.
                            ), // End this block, declaration or argument list.
                          ), // End this block, declaration or argument list.
                        ), // End this block, declaration or argument list.
                      ), // End this block, declaration or argument list.
                      leftTitles: AxisTitles( // Set the left Titles property of this widget/record.
                        axisNameWidget: // Set the axis Name Widget property of this widget/record.
                            frequencyAxis ? const Text('dBFS') : null, // Pass this next argument/value to the call or initializer above.
                        sideTitles: SideTitles( // Set the side Titles property of this widget/record.
                          showTitles: frequencyAxis, // Set the show Titles property of this widget/record.
                          interval: 30, // Set the interval property of this widget/record.
                          reservedSize: 36, // Set the reserved Size property of this widget/record.
                          getTitlesWidget: (value, meta) => SideTitleWidget( // Set the get Titles Widget property of this widget/record.
                            meta: meta, // Set the meta property of this widget/record.
                            child: Text( // Place this widget inside its parent.
                              value.round().toString(), // Call value.round with the arguments shown here.
                              style: const TextStyle( // Set the style property of this widget/record.
                                color: Colors.white70, // Choose this element's display color.
                                fontSize: 9, // Set the font Size property of this widget/record.
                              ), // End this block, declaration or argument list.
                            ), // End this block, declaration or argument list.
                          ), // End this block, declaration or argument list.
                        ), // End this block, declaration or argument list.
                      ), // End this block, declaration or argument list.
                    ), // End this block, declaration or argument list.
                    lineTouchData: const LineTouchData(enabled: false), // Set the line Touch Data property of this widget/record.
                    rangeAnnotations: RangeAnnotations( // Set the range Annotations property of this widget/record.
                      verticalRangeAnnotations: highlightedRange == null // Continue checking the file/state comparison; no value is assigned here.
                          ? const [] // Continue the expression or parameter list from the preceding line.
                          : [ // Supply the next constructor initializer or argument.
                              VerticalRangeAnnotation( // Call VerticalRangeAnnotation with the arguments shown here.
                                x1: highlightedRange.start, // Set the x1 property of this widget/record.
                                x2: highlightedRange.end, // Set the x2 property of this widget/record.
                                color: // Choose this element's display color.
                                    Colors.tealAccent.withValues(alpha: 0.10), // Pass this next argument/value to the call or initializer above.
                              ), // End this block, declaration or argument list.
                            ], // End this block, declaration or argument list.
                    ), // End this block, declaration or argument list.
                    lineBarsData: [ // Supply the plotted waveform/spectrum series.
                      if (peakSpots.isNotEmpty) // Test the condition peakSpots.isNotEmpty.
                        LineChartBarData( // Call LineChartBarData with the arguments shown here.
                          spots: peakSpots, // Supply the x/y sample points for this trace.
                          color: Colors.cyanAccent.withValues(alpha: 0.7), // Choose this element's display color.
                          barWidth: 1, // Set the bar Width property of this widget/record.
                          dotData: const FlDotData(show: false), // Set the dot Data property of this widget/record.
                        ), // End this block, declaration or argument list.
                      LineChartBarData( // Call LineChartBarData with the arguments shown here.
                        spots: spots, // Supply the x/y sample points for this trace.
                        color: color, // Choose this element's display color.
                        barWidth: 1.5, // Set the bar Width property of this widget/record.
                        dotData: const FlDotData(show: false), // Set the dot Data property of this widget/record.
                        isCurved: false, // Set the is Curved property of this widget/record.
                      ), // End this block, declaration or argument list.
                    ], // End this block, declaration or argument list.
                  ), // End this block, declaration or argument list.
                  duration: Duration.zero, // Set the duration property of this widget/record.
                ), // End this block, declaration or argument list.
              ), // End this block, declaration or argument list.
            ], // End this block, declaration or argument list.
          ), // End this block, declaration or argument list.
        ), // End this block, declaration or argument list.
      ); // End this block, declaration or argument list.
} // End this block, declaration or argument list.
