#pragma once
#include <QString>
#include <QDateTime>
#include <vector>
#include <memory>
#include <cstdint>
namespace speaker {
struct Recording { QString id, path; QDateTime date; uint64_t frames=0, bytes=0; bool missing=false, active=false, recoveryRequired=false; };
struct RecorderStatus { bool recording=false; double seconds=0; QString message, error; };
class Recorder {
public:
 Recorder(); ~Recorder();
 bool start(const QString& folder, QString& error);
 void push(const float* stereo, size_t frames) noexcept;
 void stop();
 RecorderStatus status() const;
 std::vector<Recording> recordings() const;
 bool rename(const QString& id, const QString& name, QString& error);
 bool trash(const QString& id, QString& error);
 void cleanMissing();
 QString recover();
 static QString defaultFolder();
private: struct Impl; std::unique_ptr<Impl> d;
};
}
