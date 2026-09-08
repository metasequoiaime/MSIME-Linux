#pragma once
#include <filesystem>
#include <functional>
#include <memory>
namespace metasequoia::linux_ime
{
// All participating sessions/writers share the same stable coordination directory.
// Hold Shared for the full lifetime of native dictionary references. Publication
// requires Exclusive, obtained only after releasing every local shared lease.
// Acquisition never waits on another process; nullptr means busy, errors throw.
class DictionaryLease
{
  public:
    enum class Mode
    {
        Shared,
        Exclusive
    };
    static std::unique_ptr<DictionaryLease> acquire(const std::filesystem::path &directory, Mode mode);
    // Main-thread/externally serialized use only. The host must destroy native
    // references before calling. Busy leaves this lease shared; after callback
    // success or exception it is shared again. The callback must not reenter.
    bool exclusively(const std::function<void()> &operation);
    ~DictionaryLease();
    DictionaryLease(const DictionaryLease &) = delete;
    DictionaryLease &operator=(const DictionaryLease &) = delete;

  private:
    DictionaryLease(int descriptor, int gate, Mode mode) : descriptor_(descriptor), gate_(gate), mode_(mode)
    {
    }
    int descriptor_;
    int gate_;
    Mode mode_;
    bool publishing_ = false;
};
} // namespace metasequoia::linux_ime
