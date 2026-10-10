#pragma once
#include <QString>
#include <QIcon>
#include <vector>
#include <functional>
#include <memory>
#include <cstdint>
namespace speaker {
struct Device { QString id, name; bool cable = false; };
struct Program { unsigned long pid = 0; uint64_t created = 0; QString name, path; QIcon icon; };
struct AudioConfig {
 uint64_t generation=0;
 QString microphoneId, monitorId, cableId;
 Program program;
 bool share = false, muted = false, transmit = false, monitor = false, recording = false, stable = false;
 float microphoneGain = 1, programGain = 1, monitorGain = .7f;
};
struct AudioStatus {
 uint64_t generation=0;
 bool microphoneActive=false, programActive=false, transmitActive=false, monitorActive=false;
 bool microphoneFault=false, programFault=false, transmitFault=false, monitorFault=false;
 float microphonePeak=0, programPeak=0, mixPeak=0;
 uint64_t overruns=0, underruns=0;
 double queueMilliseconds=0;
 bool clipped=false;
 QString microphoneState, programState, transmitState, monitorState, formats;
};
class AudioEngine {
public:
 using RecordSink = std::function<void(const float*, size_t)>;
 AudioEngine(); ~AudioEngine();
 static std::vector<Device> inputDevices();
 static std::vector<Device> outputDevices();
 static std::vector<Program> programs();
 bool configure(const AudioConfig&, QString& error);
 void setRecordSink(RecordSink);
 AudioStatus status() const;
 void stop();
private: struct Impl; std::unique_ptr<Impl> d;
};
}


