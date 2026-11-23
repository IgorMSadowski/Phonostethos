# Design-to-code method map

Reference: DESIGNTOEDIT, Section 3.4.2. Slides are intentionally unchanged.

The following public declarations are copied from the implemented headers in their declaration order. The corresponding .cpp definitions follow the same public-operation order. Inline definitions remain in their .h files. Private helpers support these operations and are documented in each source file.

## SystemController

Header: `app/include/phonostethos/SystemController.h`.

```cpp
explicit SystemController(const Options& options);
~SystemController();
bool initSystem();
void run();
void processCommand(const phono::protocol::CommandPacket& request);
void handleWorkerResult(const MainMessage& message);
void setMode(FilterMode mode);
void setGain(int gain);
void startRecording();
void stopRecording();
void startPlayback(uint32_t file_id);
void stopPlayback();
void deleteRecording(uint32_t file_id);
void listFiles(uint32_t cursor);
void ejectStorage();
bool initUdevMonitor();
void checkUsbEvents();
void broadcastStatus();
string generateFilename(FilterMode mode);
void stop();
```

## AudioEngine

Header: `app/include/phonostethos/AudioEngine.h`.

```cpp
AudioEngine(const Options& options, DspProcessor& dsp, atomic<unsigned>& mode, atomic<unsigned>& gain, atomic<bool>& recording, atomic<unsigned>& error_code, CommandQueue& messages, DataQueue& telemetry, pthread_mutex_t& configuration_mutex, CircularBuffer& recording_buffer, CircularBuffer& playback_buffer, atomic<uint32_t>& dropped_telemetry, atomic<uint32_t>& recording_overruns);
~AudioEngine();
bool initAlsa();
int readFrames(int16_t* buffer, int frames);
int writeFrames(const int16_t* buffer, int frames);
bool suspendCapture();
bool resumeCapture();
bool ready() const;
bool start();
void stop();
bool request(Request request);
```

## DspProcessor

Header: `app/include/phonostethos/DspProcessor.h`.

```cpp
DspProcessor();
~DspProcessor();
void applyBandpass(const float* input, float* output, int count, FilterMode mode);
void applyGainWithLimiter(const float* input, int16_t* output, int count, float gain);
void computeFFT(const int16_t* samples, float* magnitudes, int count);
bool initialize();
void reset();
bool fftReady() const;
const array<float, kFftFrames>& waveform() const;
```

## FileIoManager

Header: `app/include/phonostethos/FileIoManager.h`.

```cpp
FileIoManager(const Options& options, CommandQueue& messages, CircularBuffer& recording, CircularBuffer& playback);
~FileIoManager();
bool openRecordFile(const string& filename);
bool openPlaybackFile(const string& filename);
sf_count_t writeChunk(const int16_t* data, sf_count_t count);
sf_count_t readChunk(int16_t* data, sf_count_t count);
bool deleteRecordFile(const string& filename);
bool closeFile();
vector<string> getFileList();
bool ejectStorage();
bool start();
void stop();
bool request(Request request);
```

## NetworkManager

Header: `app/include/phonostethos/NetworkManager.h`.

```cpp
NetworkManager(const Options& options, CommandQueue& messages, DataQueue& telemetry);
~NetworkManager();
bool startListening();
bool receiveCommand(phono::protocol::CommandPacket& command);
bool sendData(const DataPacket& packet);
bool sendStatus(const phono::protocol::StatusPacket& status);
bool sendFileList(uint32_t cursor, const vector<phono::protocol::FileEntry>& entries);
void queue_control(vector<uint8_t> frame);
void start();
void stop();
bool connected() const;
```

## LedDriver

Header: `app/include/phonostethos/LedDriver.h`.

```cpp
explicit LedDriver(const std::string& path);
bool openDriver();
void closeDriver();
bool writeByte(std::uint8_t mask);
~LedDriver();
```

## CircularBuffer

Header: `app/include/phonostethos/CircularBuffer.h`.

```cpp
explicit CircularBuffer(size_t capacity = kCircularBufferSamples);
~CircularBuffer();
bool push(const int16_t* data, size_t length);
size_t pop(int16_t* destination, size_t maximum_length, chrono::milliseconds timeout = chrono::milliseconds(10));
bool isFull() const;
bool isEmpty() const;
bool begin_session();
void stop_producing();
void clear();
size_t size() const;
size_t available_capacity() const;
bool stopped() const;
```

## CommandQueue

Header: `app/include/phonostethos/CommandQueue.h`.

```cpp
CommandQueue();
~CommandQueue();
bool enqueue(phono::protocol::CommandPacket command);
bool enqueue(MainMessage message);
bool dequeue(MainMessage& message, chrono::milliseconds timeout = chrono::milliseconds(10));
void close();
void notify_all();
bool closed() const;
```

## DataQueue

Header: `app/include/phonostethos/DataQueue.h`.

```cpp
DataQueue();
~DataQueue();
bool enqueue(const DataPacket& packet);
bool dequeue(DataPacket& packet);
void set_connected(bool connected);
void clear();
```

## ControlReplyQueue

Header: `app/include/phonostethos/NetworkManager.h`.

```cpp
ControlReplyQueue();
~ControlReplyQueue();
void enqueue(vector<uint8_t> frame);
bool dequeue(vector<uint8_t>& frame);
void clear();
```

## Shared records

`RuntimeTypes.h`: Options, FilterMode, DataPacket and ArchiveIdentity.
`CommandQueue.h`: MessageKind, WorkerResultCode and MainMessage.
`protocol.h`: CommandType, CommandPacket, StatusPacket, State, CommandResult, FileEntry, FileListPacket and Frame.

TCP framing is written explicitly by serialization helpers; sending `sizeof(a_struct)` would include host padding/byte order and would be incorrect.
