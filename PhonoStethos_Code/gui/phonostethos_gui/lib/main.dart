import 'dart:math' show sqrt;
import 'dart:async';
import 'dart:io';

import 'package:fl_chart/fl_chart.dart';
import 'package:flutter/material.dart';

import 'protocol.dart';

void main() => runApp(const PhonoStethosApp());

class PhonoStethosApp extends StatelessWidget {
  const PhonoStethosApp({super.key});

  @override
  Widget build(BuildContext context) => MaterialApp(
        title: 'PhonoStethos Dashboard',
        debugShowCheckedModeBanner: false,
        theme: ThemeData.dark().copyWith(
          primaryColor: Colors.teal,
          scaffoldBackgroundColor: const Color(0xff141414),
          colorScheme: ThemeData.dark().colorScheme.copyWith(
                primary: Colors.tealAccent,
                secondary: Colors.orangeAccent,
              ),
        ),
        home: const DashboardScreen(),
      );
}

class DashboardScreen extends StatefulWidget {
  const DashboardScreen({super.key});

  @override
  State<DashboardScreen> createState() => _DashboardScreenState();
}

class _DashboardScreenState extends State<DashboardScreen> {
  static const _host = '10.42.0.3';
  static const _port = 8080;

  final _decoder = FrameDecoder();
  final _recordings = <FileEntry>[];
  Socket? _socket;
  StreamSubscription<List<int>>? _subscription;
  Timer? _reconnectTimer;
  bool _connecting = false;
  bool _connected = false;
  bool _storageMounted = false;
  bool _allowRecord = false;
  bool _allowPlayback = false;
  bool _archiveLoading = false;
  String? _errorText;
  Command? _pendingCommand;
  int? _archiveCursor;
  int _mode = 0;
  static const _gainCodes = [
    101, 102, 103, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
    11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
  ];
  int _gain = 1;
  int? _gainPreview;
  String _gainLabel(int code) => switch (code) {
    101 => '0.5×',
    102 => '0.75×',
    103 => '0.8×',
    _ => '$code×',
  };
  Transport _transport = Transport.live;
  List<FlSpot> _waveform = const [FlSpot(0, 0)];
  List<double> _spectrumPower = [];
  List<FlSpot> _spectrumPeaks = [];
  final _spectrumClock = Stopwatch()..start();
  int _lastSpectrumUs = 0;
  List<FlSpot> _spectrum = const [FlSpot(0, -120)];

  @override
  void initState() {
    super.initState();
    _connect();
  }

  Future<void> _connect() async {
    if (_connecting || _connected || !mounted) return;
    _connecting = true;
    try {
      final socket = await Socket.connect(
        _host,
        _port,
        timeout: const Duration(seconds: 3),
      );
      if (!mounted) {
        socket.destroy();
        return;
      }
      _socket = socket;
      _connected = true;
      _decoder.clear();
      _recordings.clear();
      _archiveCursor = 0;
      _pendingCommand = null;
      _errorText = null;
      setState(() {});
      _subscription = socket.listen(
        _receive,
        onError: (Object error, StackTrace stack) =>
            _connectionEnded('TCP connection failed: $error'),
        onDone: () => _connectionEnded('Disconnected from the Raspberry Pi'),
        cancelOnError: true,
      );
      _request(Command.getArchive, 0);
    } on SocketException catch (error) {
      _scheduleReconnect('Cannot connect to $_host: $_port: ${error.message}');
    } on TimeoutException {
      _scheduleReconnect('Connection to the Raspberry Pi timed out');
    } finally {
      _connecting = false;
    }
  }

  void _receive(List<int> bytes) {
    try {
      for (final message in _decoder.add(bytes)) {
        _handleMessage(message);
      }
    } on FormatException catch (error) {
      _connectionEnded('Invalid server response: ${error.message}');
    }
  }

  void _handleMessage(ServerMessage message) {
    if (!mounted) return;
    setState(() {
      switch (message) {
        case StatusMessage status:
          _storageMounted = status.storageMounted;
          _allowRecord = status.allowRecord;
          _allowPlayback = status.allowPlayback;
          if (status.errorCode != 0) {
            _errorText = 'Server error ${status.errorCode}';
          }
        case StateMessage state:
          if (_transport != state.transport || _mode != state.filter ||
              _gain != state.gain) {
            _spectrumPower = [];
            _spectrumPeaks = [];
          }
          _transport = state.transport;
          _mode = state.filter;
          _gain = state.gain;
          if (state.errorCode != 0) {
            _errorText = 'Server error ${state.errorCode}';
          }
        case DataMessage data:
          _waveform = List<FlSpot>.generate(
            data.waveform.length,
            (index) => FlSpot(index.toDouble(), data.waveform[index]),
            growable: false,
          );
          // Lightly smooth power and display every received spectrum.
          if (_spectrumPower.length != data.spectrum.length) {
            _spectrumPower = data.spectrum.map((value) => value * value).toList();
          } else {
            for (var i = 0; i < data.spectrum.length; i++) {
              final amplitude = data.spectrum[i];
              _spectrumPower[i] =
                  0.35 * _spectrumPower[i] + 0.65 * amplitude * amplitude;
            }
          }
          final visibleHz = _modeBand(_mode).end;
          final visibleBins = (visibleHz * 1024 / 8000).floor() + 1;
          final now = _spectrumClock.elapsedMicroseconds;
          final decayDb = (now - _lastSpectrumUs) * 8 / 1000000;
          _lastSpectrumUs = now;
          final previousPeaks = _spectrumPeaks;
          _spectrumPeaks = List<FlSpot>.generate(visibleBins, (index) {
            final current = spectrumDbfs(data.spectrum[index]);
            final previous = index < previousPeaks.length
                ? previousPeaks[index].y - decayDb
                : current;
            return FlSpot(index * (8000 / 1024),
                current > previous ? current : previous);
          }, growable: false);
          _spectrum = List<FlSpot>.generate(
            visibleBins,
            (index) => FlSpot(
              index * (8000 / 1024),
              spectrumDbfs(sqrt(_spectrumPower[index])),
            ),
            growable: false,
          );
        case FileListMessage files:
          final knownIds = _recordings.map((entry) => entry.id).toSet();
          _recordings.addAll(
            files.entries.where((entry) => !knownIds.contains(entry.id)),
          );
          _archiveCursor = files.nextCursor == 0 ? null : files.nextCursor;
        case CommandResultMessage result:
          if (_pendingCommand != result.command) {
            _errorText = 'Unexpected command response';
            break;
          }
          if (result.outcome == CommandOutcome.acceptedPending) {
            break;
          }
          _pendingCommand = null;
          _archiveLoading = false;
          if (result.outcome == CommandOutcome.rejected ||
              result.outcome == CommandOutcome.failed) {
            _errorText =
                'Command ${result.command.name} failed (code ${result.detail})';
            break;
          }
          _errorText = null;
          if (result.command == Command.getArchive && _archiveCursor != null) {
            final cursor = _archiveCursor!;
            _archiveCursor = null;
            WidgetsBinding.instance.addPostFrameCallback((_) {
              if (mounted) _request(Command.getArchive, cursor);
            });
          } else if (result.command == Command.deleteRecording) {
            _refreshArchive();
          }
      }
    });
  }

  // The GUI sends a request. The Pi validates it and returns CommandResult/State.
  // One pending command prevents conflicting button presses while workers finish.
  void _request(Command command, int value) {
    if (!_connected || _pendingCommand != null) return;
    final socket = _socket;
    if (socket == null) return;
    try {
      _pendingCommand = command;
      if (command == Command.getArchive) {
        _archiveLoading = true;
      }
      socket.add(encodeCommand(command, value));
      socket.flush();
      if (mounted) setState(() {});
    } on SocketException catch (error) {
      _connectionEnded('Failed to send command: ${error.message}');
    }
  }

  void _refreshArchive() {
    _recordings.clear();
    _archiveCursor = null;
    _request(Command.getArchive, 0);
  }

  // This disconnects the dashboard only; it does not stop the Pi's recording.
  void _connectionEnded(String reason) {
    if (!_connected && _reconnectTimer != null) return;
    _connected = false;
    _pendingCommand = null;
    _archiveLoading = false;
    _socket?.destroy();
    _socket = null;
    unawaited(_subscription?.cancel());
    _subscription = null;
    _scheduleReconnect(reason);
  }

  void _scheduleReconnect(String reason) {
    if (!mounted) return;
    setState(() {
      _errorText = reason;
    });
    _reconnectTimer ??= Timer(const Duration(seconds: 3), () {
      _reconnectTimer = null;
      _connect();
    });
  }

  @override
  void dispose() {
    _reconnectTimer?.cancel();
    unawaited(_subscription?.cancel());
    _socket?.destroy();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return DefaultTabController(
      length: 2,
      child: Scaffold(
        appBar: AppBar(
          title: const Text('PhonoStethos Live Clinical Dashboard'),
          backgroundColor: Colors.black45,
          actions: [
            _statusChip(
              _connected ? 'USB-C Link Active' : 'USB-C Disconnected',
              _connected ? Colors.green.shade800 : Colors.red.shade800,
            ),
            const SizedBox(width: 8),
            _statusChip(
              _storageMounted ? 'Storage Mounted' : 'No Storage',
              _storageMounted ? Colors.blue.shade800 : Colors.grey.shade700,
            ),
            const SizedBox(width: 16),
          ],
          bottom: TabBar(
            indicatorColor: Colors.tealAccent,
            labelColor: Colors.tealAccent,
            unselectedLabelColor: Colors.white60,
            tabs: [
              const Tab(
                icon: Icon(Icons.show_chart),
                text: 'Live Auscultation & Controls',
              ),
              Tab(
                icon: const Icon(Icons.folder_special),
                text: 'Recordings Archive (${_recordings.length})',
              ),
            ],
            onTap: (index) {
              if (index == 1) _refreshArchive();
            },
          ),
        ),
        body: Column(
          children: [
            if (_errorText != null)
              MaterialBanner(
                content: Text(_errorText!),
                leading: const Icon(Icons.warning_amber_rounded),
                actions: [
                  TextButton(
                    onPressed: () => setState(() => _errorText = null),
                    child: const Text('DISMISS'),
                  ),
                ],
              ),
            Expanded(
              child: TabBarView(
                children: [_buildListeningTab(), _buildArchiveTab()],
              ),
            ),
          ],
        ),
      ),
    );
  }

  Widget _statusChip(String label, Color color) => Padding(
        padding: const EdgeInsets.symmetric(vertical: 8),
        child: Chip(
          visualDensity: VisualDensity.compact,
          label: Text(label),
          side: BorderSide(color: color.withValues(alpha: 0.6)),
          backgroundColor: color.withValues(alpha: 0.12),
        ),
      );

  Widget _buildListeningTab() => LayoutBuilder(
        builder: (context, constraints) {
          final narrow = constraints.maxWidth < 850;
          final charts = Column(
            children: [
              Expanded(
                child: _chartPanel(
                  'Processed waveform',
                  '128 ms history · normalized PCM16',
                  _waveform,
                  Colors.tealAccent,
                  minY: -1,
                  maxY: 1,
                  maxX: 1024,
                ),
              ),
              const SizedBox(height: 12),
              Expanded(
                child: _chartPanel(
                  'Frequency spectrum · ${_modeName(_mode)}',
                  '${_bandDescription(_mode)} · orange: live · cyan: recent peaks',
                  _spectrum,
                  Colors.orangeAccent,
                  minY: -90,
                  maxY: 0,
                  maxX: _modeBand(_mode).end,
                  peakSpots: _spectrumPeaks,
                  highlightedRange: _modeBand(_mode),
                  frequencyAxis: true,
                ),
              ),
            ],
          );
          final controls = _buildControls();
          return Padding(
            padding: const EdgeInsets.all(16),
            child: narrow
                ? ListView(
                    children: [
                      SizedBox(height: 560, child: charts),
                      const SizedBox(height: 16),
                      controls,
                    ],
                  )
                : Row(
                    children: [
                      Expanded(flex: 3, child: charts),
                      const SizedBox(width: 16),
                      SizedBox(width: 340, child: controls),
                    ],
                  ),
          );
        },
      );

  Widget _buildControls() => Card(
        child: ListView(
          padding: const EdgeInsets.all(20),
          children: [
            Text('Transport: ${_transport.name.toUpperCase()}'),
            const SizedBox(height: 16),
            const Text(
              'DSP Filter Mode',
              style: TextStyle(fontSize: 16, fontWeight: FontWeight.bold),
            ),
            const SizedBox(height: 12),
            _buildModeButton(0, 'General (20 - 2000 Hz)', Colors.blue),
            _buildModeButton(1, 'Heart Murmurs (20 - 600 Hz)', Colors.red),
            _buildModeButton(2, 'Lung Sounds (100 - 1000 Hz)', Colors.amber),
            const SizedBox(height: 8),
            const Divider(),
            Row(
              children: [
                const Expanded(child: Text('Gain (live and playback)')),
                Text(_gainLabel(_gainPreview ?? _gain)),
              ],
            ),
            Slider(
              value: _gainCodes.indexOf(_gainPreview ?? _gain)
                  .clamp(0, _gainCodes.length - 1).toDouble(),
              max: (_gainCodes.length - 1).toDouble(),
              divisions: _gainCodes.length - 1,
              label: _gainLabel(_gainPreview ?? _gain),
              onChanged: _enabled
                  ? (value) => setState(() => _gainPreview = _gainCodes[value.round()])
                  : null,
              onChangeEnd: _enabled ? (value) {
                setState(() => _gainPreview = null);
                _sendGain(_gainCodes[value.round()]);
              } : null,
            ),
            const Text('Live Audio Capture'),
            const SizedBox(height: 12),
            FilledButton.icon(
              onPressed: !_enabled
                  ? null
                  : _transport == Transport.recording
                      ? () => _request(Command.stopRecording, 0)
                      : _allowRecord && _transport == Transport.live
                          ? () => _request(Command.startRecording, 0)
                          : null,
              icon: Icon(
                _transport == Transport.recording
                    ? Icons.stop_circle_outlined
                    : Icons.fiber_manual_record,
              ),
              label: Text(
                _transport == Transport.recording
                    ? 'Stop Recording'
                    : 'Record Auscultation',
              ),
            ),
            const SizedBox(height: 8),
            OutlinedButton.icon(
              onPressed: _enabled && _transport == Transport.playback
                  ? () => _request(Command.stopPlayback, 0)
                  : null,
              icon: const Icon(Icons.stop),
              label: const Text('Stop playback'),
            ),
            const SizedBox(height: 8),
            OutlinedButton.icon(
              onPressed: _enabled && _storageMounted
                  ? () => _request(Command.ejectStorage, 0)
                  : null,
              icon: const Icon(Icons.eject),
              label: const Text('Eject USB Safely'),
            ),
          ],
        ),
      );

  bool get _enabled => _connected && _pendingCommand == null;

  void _sendMode(int value) {
    setState(() => _mode = value);
    _request(Command.setMode, value);
  }

  void _sendGain(int value) {
    _request(Command.setGain, value);
  }

  Widget _buildModeButton(int mode, String label, Color activeColor) => Padding(
        padding: const EdgeInsets.only(bottom: 8),
        child: ElevatedButton(
          style: ElevatedButton.styleFrom(
            backgroundColor: _mode == mode ? activeColor : Colors.grey.shade900,
            foregroundColor: Colors.white,
            minimumSize: const Size.fromHeight(42),
          ),
          onPressed: _enabled ? () => _sendMode(mode) : null,
          child: Text(label, style: const TextStyle(fontSize: 14)),
        ),
      );

  String _modeName(int mode) =>
      const ['General', 'Heart', 'Lung'][mode.clamp(0, 2)];

  RangeValues _modeBand(int mode) => switch (mode) {
        1 => const RangeValues(20, 600),
        2 => const RangeValues(100, 1000),
        _ => const RangeValues(20, 2000),
      };

  String _bandDescription(int mode) {
    final band = _modeBand(mode);
    return '${band.start.round()}–${band.end.round()} Hz passband';
  }

  Widget _buildArchiveTab() => Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            Row(
              children: [
                Text(
                  'USB recordings',
                  style: Theme.of(context).textTheme.titleLarge,
                ),
                const Spacer(),
                if (_archiveLoading)
                  const SizedBox.square(
                    dimension: 22,
                    child: CircularProgressIndicator(strokeWidth: 2),
                  ),
                IconButton(
                  tooltip: 'Refresh archive',
                  onPressed: _enabled ? _refreshArchive : null,
                  icon: const Icon(Icons.refresh),
                ),
              ],
            ),
            const SizedBox(height: 8),
            Expanded(
              child: _recordings.isEmpty
                  ? const Center(child: Text('No recordings in the archive'))
                  : ListView.separated(
                      itemCount: _recordings.length,
                      separatorBuilder: (_, __) => const Divider(height: 1),
                      itemBuilder: (context, index) {
                        final file = _recordings[index];
                        return ListTile(
                          leading: const Icon(Icons.audio_file_outlined),
                          title: Text(file.basename),
                          subtitle: Text('File ID ${file.id}'),
                          trailing: Wrap(
                            spacing: 4,
                            children: [
                              IconButton(
                                tooltip: 'Play recording',
                                onPressed: _enabled &&
                                        _allowPlayback &&
                                        _transport == Transport.live
                                    ? () {
                                        _request(Command.playAudio, file.id);
                                      }
                                    : null,
                                icon: const Icon(Icons.play_arrow),
                              ),
                              IconButton(
                                tooltip: 'Delete recording',
                                onPressed: _enabled
                                    ? () => _confirmDelete(file)
                                    : null,
                                icon: const Icon(Icons.delete_outline),
                              ),
                            ],
                          ),
                        );
                      },
                    ),
            ),
          ],
        ),
      );

  Future<void> _confirmDelete(FileEntry file) async {
    final confirmed = await showDialog<bool>(
      context: context,
      builder: (context) => AlertDialog(
        title: const Text('Delete recording?'),
        content: Text('Permanently delete ${file.basename}?'),
        actions: [
          TextButton(
            onPressed: () => Navigator.pop(context, false),
            child: const Text('Cancel'),
          ),
          FilledButton(
            onPressed: () => Navigator.pop(context, true),
            child: const Text('Delete'),
          ),
        ],
      ),
    );
    if (confirmed == true && mounted) {
      _request(Command.deleteRecording, file.id);
    }
  }

  Widget _chartPanel(
    String title,
    String subtitle,
    List<FlSpot> spots,
    Color color, {
    required double minY,
    required double maxY,
    required double maxX,
    RangeValues? highlightedRange,
    bool frequencyAxis = false,
    List<FlSpot> peakSpots = const [],
  }) =>
      Card(
        child: Padding(
          padding: const EdgeInsets.fromLTRB(16, 12, 16, 8),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Text(title, style: Theme.of(context).textTheme.titleMedium),
              Text(subtitle, style: Theme.of(context).textTheme.bodySmall),
              const SizedBox(height: 8),
              Expanded(
                child: LineChart(
                  LineChartData(
                    minX: 0,
                    maxX: maxX,
                    minY: minY,
                    maxY: maxY,
                    gridData:
                        const FlGridData(show: true, drawVerticalLine: false),
                    borderData: FlBorderData(show: false),
                    titlesData: const FlTitlesData(
                      topTitles: AxisTitles(
                        sideTitles: SideTitles(showTitles: false),
                      ),
                      rightTitles: AxisTitles(
                        sideTitles: SideTitles(showTitles: false),
                      ),
                    ).copyWith(
                      bottomTitles: AxisTitles(
                        axisNameWidget:
                            frequencyAxis ? const Text('Frequency (Hz)') : null,
                        sideTitles: SideTitles(
                          showTitles: frequencyAxis,
                          interval: maxX <= 600 ? 100 : maxX / 4,
                          reservedSize: 28,
                          getTitlesWidget: (value, meta) => SideTitleWidget(
                            meta: meta,
                            child: Text(
                              value.round().toString(),
                              style: const TextStyle(
                                color: Colors.white70,
                                fontSize: 9,
                              ),
                            ),
                          ),
                        ),
                      ),
                      leftTitles: AxisTitles(
                        axisNameWidget:
                            frequencyAxis ? const Text('dBFS') : null,
                        sideTitles: SideTitles(
                          showTitles: frequencyAxis,
                          interval: 30,
                          reservedSize: 36,
                          getTitlesWidget: (value, meta) => SideTitleWidget(
                            meta: meta,
                            child: Text(
                              value.round().toString(),
                              style: const TextStyle(
                                color: Colors.white70,
                                fontSize: 9,
                              ),
                            ),
                          ),
                        ),
                      ),
                    ),
                    lineTouchData: const LineTouchData(enabled: false),
                    rangeAnnotations: RangeAnnotations(
                      verticalRangeAnnotations: highlightedRange == null
                          ? const []
                          : [
                              VerticalRangeAnnotation(
                                x1: highlightedRange.start,
                                x2: highlightedRange.end,
                                color:
                                    Colors.tealAccent.withValues(alpha: 0.10),
                              ),
                            ],
                    ),
                    lineBarsData: [
                      if (peakSpots.isNotEmpty)
                        LineChartBarData(
                          spots: peakSpots,
                          color: Colors.cyanAccent.withValues(alpha: 0.7),
                          barWidth: 1,
                          dotData: const FlDotData(show: false),
                        ),
                      LineChartBarData(
                        spots: spots,
                        color: color,
                        barWidth: 1.5,
                        dotData: const FlDotData(show: false),
                        isCurved: false,
                      ),
                    ],
                  ),
                  duration: Duration.zero,
                ),
              ),
            ],
          ),
        ),
      );
}
