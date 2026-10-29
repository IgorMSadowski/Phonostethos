import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';
import 'package:flutter/material.dart';
import 'package:fl_chart/fl_chart.dart';

void main() {
  runApp(const PhonoStethosApp());
}

class PhonoStethosApp extends StatelessWidget {
  const PhonoStethosApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'PhonoStethos Dashboard',
      debugShowCheckedModeBanner: false,
      theme: ThemeData.dark().copyWith(
        primaryColor: Colors.teal,
        scaffoldBackgroundColor: const Color(0xFF141414),
      ),
      home: const DashboardScreen(),
    );
  }
}

class DashboardScreen extends StatefulWidget {
  const DashboardScreen({super.key});

  @override
  State<DashboardScreen> createState() => _DashboardScreenState();
}

class _DashboardScreenState extends State<DashboardScreen> {
  // ==========================================
  // TCP SOCKET & NETWORK STATE (USB-C GADGET)
  // ==========================================
  Socket? _socket;
  bool _isConnected = false;
  final List<int> _rxBuffer = []; // Buffer contra fragmentação de streams TCP

  // Estado do Sistema (Notificado via StatusPacket)
  bool _usbConnected = false;
  bool _isRecording = false;
  bool _isPlaying = false;
  String? _currentlyPlayingFile;
  int _errorCode = 0;

  // Estado da Interface e Filtros
  int _currentMode = 0; // 0=General, 1=Heart, 2=Lung
  double _currentGain = 1.0;

  // Dados dos Gráficos em Tempo Real
  List<FlSpot> _waveformSpots = [const FlSpot(0, 0)];
  List<FlSpot> _fftSpots = [const FlSpot(0, 0)];

  // Lista de Gravações (.wav na Pen USB)
  List<String> _recordingsList = [];

  @override
  void initState() {
    super.initState();
    _connectToRaspberryPi();
  }

  // ==========================================
  // CAMADA DE REDE (TCP OVER VIRTUAL ETHERNET)
  // ==========================================
  Future<void> _connectToRaspberryPi() async {
    try {
      // IP estático da Pi no barramento USB-C Virtual Ethernet (usb0)
      _socket = await Socket.connect('10.42.0.3', 8080,
          timeout: const Duration(seconds: 3));
      setState(() => _isConnected = true);

      // Pede o arquivo de ficheiros ao ligar
      _sendCommand(8, 0); // 8 = GET_ARCHIVE

      _socket!.listen(
        (Uint8List data) {
          _rxBuffer.addAll(data);
          _processRxBuffer();
        },
        onDone: () {
          setState(() {
            _isConnected = false;
            _isPlaying = false;
          });
          _reconnect();
        },
        onError: (e) {
          setState(() {
            _isConnected = false;
            _isPlaying = false;
          });
          _reconnect();
        },
      );
    } catch (e) {
      setState(() => _isConnected = false);
      _reconnect();
    }
  }

  void _reconnect() {
    if (!_isConnected) {
      Timer(const Duration(seconds: 3), _connectToRaspberryPi);
    }
  }

  void _processRxBuffer() {
    while (_rxBuffer.isNotEmpty) {
      int msgType = _rxBuffer[0];

      if (msgType == 0x01) {
        // DataPacket: 1 (tipo) + 4 (tamanho) + 4096 (1024 floats áudio) + 2048 (512 floats FFT) = 6149 bytes
        if (_rxBuffer.length < 6149) return;

        _parseDataPacket(Uint8List.fromList(_rxBuffer.sublist(0, 6149)));
        _rxBuffer.removeRange(0, 6149);
      } else if (msgType == 0x02) {
        // StatusPacket: 1 (tipo) + 4 bytes payload = 5 bytes
        if (_rxBuffer.length < 5) return;

        _parseStatusPacket(Uint8List.fromList(_rxBuffer.sublist(0, 5)));
        _rxBuffer.removeRange(0, 5);
      } else if (msgType == 0x03) {
        // FileListPacket: 1 (tipo) + 4 (tamanho string) + N (nomes UTF-8 com \n)
        if (_rxBuffer.length < 5) return;

        ByteData bd =
            ByteData.sublistView(Uint8List.fromList(_rxBuffer.sublist(0, 5)));
        int payloadLen = bd.getUint32(1, Endian.big);
        int totalPacketLen = 5 + payloadLen;

        if (_rxBuffer.length < totalPacketLen) return;

        Uint8List payloadBytes =
            Uint8List.fromList(_rxBuffer.sublist(5, totalPacketLen));
        _parseFileListPacket(payloadBytes);
        _rxBuffer.removeRange(0, totalPacketLen);
      } else {
        _rxBuffer.removeAt(0);
      }
    }
  }

  void _parseDataPacket(Uint8List packet) {
    ByteData byteData = ByteData.sublistView(packet);
    List<FlSpot> newWaveform = [];
    List<FlSpot> newFft = [];

    // 1024 floats de áudio no tempo (offset 5, Little Endian)
    for (int i = 0; i < 1024; i++) {
      double val = byteData.getFloat32(5 + (i * 4), Endian.little);
      newWaveform.add(FlSpot(i.toDouble(), val));
    }

    // 512 magnitudes FFT na frequência (offset 4101, Little Endian)
    for (int i = 0; i < 512; i++) {
      double val = byteData.getFloat32(4101 + (i * 4), Endian.little);
      newFft.add(FlSpot(i.toDouble(), val));
    }

    setState(() {
      _waveformSpots = newWaveform;
      _fftSpots = newFft;
    });
  }

  void _parseStatusPacket(Uint8List packet) {
    setState(() {
      _usbConnected = packet[1] == 1;
      _errorCode = packet[4];
    });

    if (_errorCode == 1 || _errorCode == 2) {
      _showErrorBanner("Storage Error! Please check USB Flash Drive.");
      setState(() {
        _isRecording = false;
        _isPlaying = false;
      });
    }
  }

  void _parseFileListPacket(Uint8List payload) {
    String rawData = utf8.decode(payload);
    List<String> files = rawData
        .split('\n')
        .map((f) => f.trim())
        .where((f) => f.isNotEmpty && f.endsWith('.wav'))
        .toList();

    setState(() {
      _recordingsList = files;
    });
  }

  void _sendCommand(int cmdType, int value) {
    if (_socket == null || !_isConnected) return;

    ByteData data = ByteData(5);
    data.setUint8(0, cmdType);
    data.setUint32(1, value, Endian.big); // Network Byte Order (htonl)

    _socket!.add(data.buffer.asUint8List());
  }

  void _showErrorBanner(String msg) {
    ScaffoldMessenger.of(context).showSnackBar(SnackBar(
      content: Text(msg),
      backgroundColor: Colors.red[800],
      duration: const Duration(seconds: 3),
    ));
  }

  void _confirmDeleteRecording(int index, String filename) {
    showDialog(
      context: context,
      builder: (BuildContext ctx) {
        return AlertDialog(
          backgroundColor: const Color(0xFF2A2A2A),
          shape:
              RoundedRectangleBorder(borderRadius: BorderRadius.circular(12)),
          title: const Row(
            children: [
              Icon(Icons.warning_amber_rounded, color: Colors.redAccent),
              SizedBox(width: 8),
              Text("Delete Recording?",
                  style: TextStyle(color: Colors.white, fontSize: 18)),
            ],
          ),
          content: Text(
            "Are you sure you want to permanently delete:\n\n$filename\n\nThis action cannot be undone.",
            style: const TextStyle(color: Colors.white70, fontSize: 14),
          ),
          actions: [
            TextButton(
              onPressed: () => Navigator.of(ctx).pop(),
              child: const Text("CANCEL", style: TextStyle(color: Colors.grey)),
            ),
            ElevatedButton(
              style: ElevatedButton.styleFrom(
                backgroundColor: Colors.red[800],
                foregroundColor: Colors.white,
              ),
              onPressed: () {
                Navigator.of(ctx).pop();

                if (_isPlaying && _currentlyPlayingFile == filename) {
                  _sendCommand(6, 0); // 6 = STOP_PLAYBACK
                  setState(() {
                    _isPlaying = false;
                    _currentlyPlayingFile = null;
                  });
                }

                _sendCommand(9, index); // 9 = DELETE_RECORDING

                setState(() {
                  _recordingsList.removeAt(index);
                });

                Future.delayed(const Duration(milliseconds: 300), () {
                  _sendCommand(8, 0); // 8 = GET_ARCHIVE refresh
                });
              },
              child: const Text("DELETE"),
            ),
          ],
        );
      },
    );
  }

  // ==========================================
  // APRESENTAÇÃO / ESTRUTURA GERAL
  // ==========================================
  @override
  Widget build(BuildContext context) {
    return DefaultTabController(
      length: 2,
      child: Scaffold(
        appBar: AppBar(
          title: const Text('PhonoStethos Live Clinical Dashboard'),
          backgroundColor: Colors.black45,
          actions: [
            // Status do cabo USB-C (Virtual Ethernet)
            Chip(
              avatar: Icon(
                _isConnected ? Icons.usb : Icons.usb_off,
                color: Colors.white,
                size: 18,
              ),
              label: Text(
                  _isConnected ? "USB-C Link Active" : "USB-C Disconnected"),
              backgroundColor:
                  _isConnected ? Colors.green[800] : Colors.red[800],
            ),
            const SizedBox(width: 10),
            // Status da Pen USB
            Chip(
              avatar: Icon(
                _usbConnected ? Icons.storage : Icons.disc_full,
                color: Colors.white,
                size: 18,
              ),
              label: Text(_usbConnected ? "Storage Mounted" : "No Storage"),
              backgroundColor:
                  _usbConnected ? Colors.blue[800] : Colors.grey[700],
            ),
            const SizedBox(width: 20),
          ],
          // Abas principais no topo
          bottom: TabBar(
            indicatorColor: Colors.tealAccent,
            indicatorWeight: 3,
            labelColor: Colors.tealAccent,
            unselectedLabelColor: Colors.white60,
            tabs: [
              const Tab(
                icon: Icon(Icons.show_chart),
                text: "Live Auscultation & Controls",
              ),
              Tab(
                icon: const Icon(Icons.folder_special),
                text: "Recordings Archive (${_recordingsList.length})",
              ),
            ],
            onTap: (index) {
              // Se o médico entrar na aba de arquivo, pede a lista atualizada
              if (index == 1) {
                _sendCommand(8, 0); // 8 = GET_ARCHIVE
              }
            },
          ),
        ),
        body: TabBarView(
          children: [
            // VISTA 1: Auscultação ao vivo (Gráficos + Controlos)
            _buildLiveAuscultationView(),

            // VISTA 2: Arquivo Clínico em Ecrã Inteiro (sem gráficos pretos!)
            _buildFullScreenArchiveView(),
          ],
        ),
      ),
    );
  }

  // ==========================================
  // VISTA 1: LIVE AUSCULTATION (GRÁFICOS + CONTROLOS)
  // ==========================================
  Widget _buildLiveAuscultationView() {
    return Padding(
      padding: const EdgeInsets.all(16.0),
      child: Row(
        children: [
          // Gráficos ao vivo
          Expanded(
            flex: 3,
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.stretch,
              children: [
                const Text("Oscilloscope (Time Domain)",
                    style: TextStyle(
                        fontSize: 16,
                        fontWeight: FontWeight.bold,
                        color: Colors.tealAccent)),
                const SizedBox(height: 6),
                Expanded(
                    child: _buildChart(
                        _waveformSpots, -32768.0, 32767.0, Colors.tealAccent)),
                const SizedBox(height: 16),
                const Text("FFT Spectrum (Frequency Domain)",
                    style: TextStyle(
                        fontSize: 16,
                        fontWeight: FontWeight.bold,
                        color: Colors.orangeAccent)),
                const SizedBox(height: 6),
                Expanded(
                    child:
                        _buildChart(_fftSpots, 0, 1000.0, Colors.orangeAccent)),
              ],
            ),
          ),
          const SizedBox(width: 20),
          // Painel de controlo
          Expanded(
            flex: 1,
            child: Container(
              padding: const EdgeInsets.all(16),
              decoration: BoxDecoration(
                  color: Colors.grey[900],
                  borderRadius: BorderRadius.circular(12)),
              child: SingleChildScrollView(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    const Text("DSP Filter Mode",
                        style: TextStyle(
                            fontSize: 16, fontWeight: FontWeight.bold)),
                    const SizedBox(height: 10),
                    _buildModeButton(0, "General (20 - 2000 Hz)", Colors.blue),
                    _buildModeButton(
                        1, "Heart Murmurs (20 - 600 Hz)", Colors.red),
                    _buildModeButton(
                        2, "Lung Sounds (100 - 1000 Hz)", Colors.amber[800]!),
                    const Divider(height: 30, color: Colors.grey),
                    const Text("Digital Dynamic Gain",
                        style: TextStyle(
                            fontSize: 16, fontWeight: FontWeight.bold)),
                    Row(
                      children: [
                        Expanded(
                          child: Slider(
                            value: _currentGain,
                            min: 1,
                            max: 10,
                            divisions: 9,
                            activeColor: Colors.tealAccent,
                            label: "${_currentGain.toInt()}x",
                            onChanged: (val) {
                              setState(() => _currentGain = val);
                              _sendCommand(2, val.toInt()); // 2 = SET_GAIN
                            },
                          ),
                        ),
                        Text("${_currentGain.toInt()}x",
                            style:
                                const TextStyle(fontWeight: FontWeight.bold)),
                      ],
                    ),
                    const Divider(height: 30, color: Colors.grey),
                    const Text("Live Audio Capture",
                        style: TextStyle(
                            fontSize: 16, fontWeight: FontWeight.bold)),
                    const SizedBox(height: 10),
                    ElevatedButton.icon(
                      style: ElevatedButton.styleFrom(
                        backgroundColor:
                            _isRecording ? Colors.red[700] : Colors.green[700],
                        padding: const EdgeInsets.symmetric(vertical: 14),
                        minimumSize: const Size.fromHeight(50),
                      ),
                      icon: Icon(
                          _isRecording ? Icons.stop : Icons.fiber_manual_record,
                          color: Colors.white),
                      label: Text(
                          _isRecording
                              ? "Stop Recording"
                              : "Record Auscultation",
                          style: const TextStyle(
                              color: Colors.white, fontSize: 15)),
                      onPressed: () {
                        if (!_usbConnected && !_isRecording) {
                          _showErrorBanner(
                              "Cannot record: USB Flash Drive not mounted!");
                          return;
                        }
                        setState(() => _isRecording = !_isRecording);
                        _sendCommand(
                            _isRecording ? 3 : 4, 0); // 3=START, 4=STOP

                        if (!_isRecording) {
                          Future.delayed(const Duration(milliseconds: 500),
                              () => _sendCommand(8, 0));
                        }
                      },
                    ),
                    const SizedBox(height: 20),
                    SizedBox(
                      width: double.infinity,
                      height: 45,
                      child: OutlinedButton.icon(
                        style: OutlinedButton.styleFrom(
                          foregroundColor: Colors.white70,
                          side: const BorderSide(color: Colors.white24),
                        ),
                        icon: const Icon(Icons.eject),
                        label: const Text("Eject USB Safely"),
                        onPressed: () => _sendCommand(7, 0), // 7 = EJECT_USB
                      ),
                    )
                  ],
                ),
              ),
            ),
          )
        ],
      ),
    );
  }

  Widget _buildChart(
      List<FlSpot> spots, double minY, double maxY, Color traceColor) {
    return Container(
      padding: const EdgeInsets.all(10),
      decoration: BoxDecoration(
        color: Colors.black,
        border: Border.all(color: Colors.grey[850]!, width: 2),
        borderRadius: BorderRadius.circular(8),
      ),
      child: LineChart(
        LineChartData(
          minY: minY,
          maxY: maxY,
          titlesData: const FlTitlesData(show: false),
          borderData: FlBorderData(show: false),
          gridData: FlGridData(
            show: true,
            drawVerticalLine: true,
            getDrawingHorizontalLine: (value) =>
                const FlLine(color: Colors.white10, strokeWidth: 1),
          ),
          lineBarsData: [
            LineChartBarData(
              spots: spots,
              isCurved: false,
              color: traceColor,
              dotData: const FlDotData(show: false),
              belowBarData: BarAreaData(
                show: true,
                color: traceColor.withValues(alpha: 0.08),
              ),
            )
          ],
        ),
      ),
    );
  }

  Widget _buildModeButton(int modeIndex, String title, Color activeColor) {
    bool isSelected = _currentMode == modeIndex;
    return Padding(
      padding: const EdgeInsets.only(bottom: 8.0),
      child: ElevatedButton(
        style: ElevatedButton.styleFrom(
          backgroundColor: isSelected ? activeColor : Colors.grey[850],
          foregroundColor: isSelected ? Colors.white : Colors.grey[300],
          minimumSize: const Size.fromHeight(42),
        ),
        onPressed: () {
          setState(() => _currentMode = modeIndex);
          _sendCommand(1, modeIndex); // 1 = SET_MODE
        },
        child: Text(title, style: const TextStyle(fontSize: 14)),
      ),
    );
  }

  // ==========================================
  // VISTA 2: ARQUIVO CLÍNICO EM ECRÃ INTEIRO
  // ==========================================
  Widget _buildFullScreenArchiveView() {
    return Padding(
      padding: const EdgeInsets.all(24.0),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          // Cabeçalho da página de Arquivo
          Row(
            mainAxisAlignment: MainAxisAlignment.spaceBetween,
            children: [
              Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  const Text("Stored Auscultation Sessions",
                      style:
                          TextStyle(fontSize: 22, fontWeight: FontWeight.bold)),
                  const SizedBox(height: 4),
                  Text(
                    _usbConnected
                        ? "Storage location: /media/usb_storage/ (Direct WAV playback)"
                        : "Flash Drive not connected. Please insert USB drive into Raspberry Pi.",
                    style: TextStyle(
                        color:
                            _usbConnected ? Colors.grey[400] : Colors.red[300]),
                  ),
                ],
              ),
              ElevatedButton.icon(
                style: ElevatedButton.styleFrom(
                  backgroundColor: Colors.teal[800],
                  foregroundColor: Colors.white,
                  padding:
                      const EdgeInsets.symmetric(horizontal: 16, vertical: 12),
                ),
                icon: const Icon(Icons.refresh),
                label: const Text("Refresh List"),
                onPressed: () => _sendCommand(8, 0), // 8 = GET_ARCHIVE
              )
            ],
          ),
          const SizedBox(height: 16),

          // Banner de reprodução ativa de áudio
          if (_isPlaying)
            Container(
              margin: const EdgeInsets.only(bottom: 16),
              padding: const EdgeInsets.symmetric(horizontal: 20, vertical: 14),
              decoration: BoxDecoration(
                color: Colors.orange[900]!.withValues(alpha: 0.35),
                borderRadius: BorderRadius.circular(10),
                border: Border.all(color: Colors.orangeAccent),
              ),
              child: Row(
                children: [
                  const Icon(Icons.volume_up,
                      color: Colors.orangeAccent, size: 28),
                  const SizedBox(width: 14),
                  Expanded(
                    child: Column(
                      crossAxisAlignment: CrossAxisAlignment.start,
                      children: [
                        const Text(
                          "STREAMING TO HEADPHONES VIA ALSA",
                          style: TextStyle(
                              color: Colors.orangeAccent,
                              fontSize: 11,
                              fontWeight: FontWeight.bold),
                        ),
                        Text(
                          _currentlyPlayingFile ?? '',
                          style: const TextStyle(
                              color: Colors.white,
                              fontSize: 15,
                              fontWeight: FontWeight.bold),
                          overflow: TextOverflow.ellipsis,
                        ),
                      ],
                    ),
                  ),
                  ElevatedButton.icon(
                    style: ElevatedButton.styleFrom(
                      backgroundColor: Colors.red[800],
                      foregroundColor: Colors.white,
                    ),
                    icon: const Icon(Icons.stop),
                    label: const Text("STOP PLAYBACK"),
                    onPressed: () {
                      _sendCommand(6, 0); // 6 = STOP_PLAYBACK
                      setState(() {
                        _isPlaying = false;
                        _currentlyPlayingFile = null;
                      });
                    },
                  )
                ],
              ),
            ),

          // Lista de Ficheiros
          Expanded(
            child: _recordingsList.isEmpty
                ? Center(
                    child: Column(
                      mainAxisAlignment: MainAxisAlignment.center,
                      children: [
                        Icon(
                          _usbConnected
                              ? Icons.audio_file_outlined
                              : Icons.usb_off_outlined,
                          size: 64,
                          color: Colors.grey[600],
                        ),
                        const SizedBox(height: 16),
                        Text(
                          _usbConnected
                              ? "No audio sessions recorded yet.\nSwitch to 'Live Auscultation' and tap 'Record'."
                              : "USB Flash Storage is not mounted.\nInsert a drive into the Raspberry Pi.",
                          textAlign: TextAlign.center,
                          style:
                              TextStyle(color: Colors.grey[500], fontSize: 16),
                        ),
                      ],
                    ),
                  )
                : Container(
                    decoration: BoxDecoration(
                      color: Colors.grey[900],
                      borderRadius: BorderRadius.circular(12),
                      border: Border.all(color: Colors.white10),
                    ),
                    child: ListView.separated(
                      padding: const EdgeInsets.symmetric(vertical: 8),
                      itemCount: _recordingsList.length,
                      separatorBuilder: (context, index) =>
                          const Divider(height: 1, color: Colors.white12),
                      itemBuilder: (context, index) {
                        final filename = _recordingsList[index];
                        final isThisPlaying =
                            _isPlaying && _currentlyPlayingFile == filename;

                        // Extrai o modo e cor a partir do formato [MODE]_[DATA]_[HORA].wav
                        Color badgeColor = Colors.blue;
                        String modeName = "GENERAL";
                        if (filename.startsWith("HEART")) {
                          badgeColor = Colors.redAccent;
                          modeName = "HEART";
                        } else if (filename.startsWith("LUNG")) {
                          badgeColor = Colors.amber[700]!;
                          modeName = "LUNG";
                        }

                        return ListTile(
                          contentPadding: const EdgeInsets.symmetric(
                              horizontal: 20, vertical: 8),
                          leading: CircleAvatar(
                            backgroundColor: badgeColor.withValues(alpha: 0.2),
                            child: Icon(
                              isThisPlaying
                                  ? Icons.graphic_eq
                                  : Icons.audiotrack,
                              color: isThisPlaying
                                  ? Colors.orangeAccent
                                  : badgeColor,
                            ),
                          ),
                          title: Row(
                            children: [
                              Container(
                                padding: const EdgeInsets.symmetric(
                                    horizontal: 8, vertical: 2),
                                decoration: BoxDecoration(
                                  color: badgeColor,
                                  borderRadius: BorderRadius.circular(4),
                                ),
                                child: Text(
                                  modeName,
                                  style: const TextStyle(
                                      fontSize: 10,
                                      fontWeight: FontWeight.bold,
                                      color: Colors.black),
                                ),
                              ),
                              const SizedBox(width: 12),
                              Expanded(
                                child: Text(
                                  filename,
                                  style: TextStyle(
                                    fontSize: 14,
                                    fontWeight: isThisPlaying
                                        ? FontWeight.bold
                                        : FontWeight.w500,
                                    color: isThisPlaying
                                        ? Colors.orangeAccent
                                        : Colors.white,
                                  ),
                                  overflow: TextOverflow.ellipsis,
                                ),
                              ),
                            ],
                          ),
                          trailing: Row(
                            mainAxisSize: MainAxisSize.min,
                            children: [
                              // Play / Stop
                              IconButton(
                                icon: Icon(
                                  isThisPlaying
                                      ? Icons.stop_circle
                                      : Icons.play_circle_fill,
                                  size: 32,
                                  color: isThisPlaying
                                      ? Colors.redAccent
                                      : Colors.tealAccent,
                                ),
                                tooltip: isThisPlaying
                                    ? "Stop Playback"
                                    : "Play to Headphones",
                                onPressed: () {
                                  if (isThisPlaying) {
                                    _sendCommand(6, 0); // STOP_PLAYBACK
                                    setState(() {
                                      _isPlaying = false;
                                      _currentlyPlayingFile = null;
                                    });
                                  } else {
                                    _sendCommand(5, index); // PLAY_AUDIO
                                    setState(() {
                                      _isPlaying = true;
                                      _currentlyPlayingFile = filename;
                                    });
                                  }
                                },
                              ),
                              const SizedBox(width: 8),
                              // Delete
                              IconButton(
                                icon: const Icon(
                                  Icons.delete_outline,
                                  color: Colors.redAccent,
                                  size: 24,
                                ),
                                tooltip: "Delete recording",
                                onPressed: () =>
                                    _confirmDeleteRecording(index, filename),
                              ),
                            ],
                          ),
                        );
                      },
                    ),
                  ),
          ),
        ],
      ),
    );
  }

  @override
  void dispose() {
    _socket?.close();
    super.dispose();
  }
}
