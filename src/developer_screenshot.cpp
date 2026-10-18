#include "developer_screenshot.h"
#include "developer_lifecycle.h"
#include "settings.h"
#include "glad/glad.h"
#include <game_window.h>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <memory>
#include <mutex>
#include <regex>
#include <stdexcept>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace DeveloperScreenshot {
namespace {
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
constexpr size_t maxBytes = 16 * 1024 * 1024 * 4;
struct Frame {
    std::string id, path, state = "queued", code;
    int fd = -1;
    void* pixels = MAP_FAILED;
    int width = 0, height = 0;
    int64_t timestamp = 0;
    bool released = false, published = false;
    Clock::time_point deadline = Clock::now() + std::chrono::seconds(5);
    Clock::time_point expires = Clock::now() + std::chrono::seconds(30);

    ~Frame() {
        if(pixels != MAP_FAILED) munmap(pixels, maxBytes);
        if(fd >= 0) close(fd);
        if(!path.empty()) unlink(path.c_str());
    }
};
std::string directory;
std::mutex mutex;
std::shared_ptr<Frame> current;
std::atomic<bool> pending{false};

Json failure(const std::string& code) { return {{"state", "failed"}, {"code", code}}; }

Json result(Frame& frame) {
    if(frame.released) return {{"state", "cancelled"}, {"request_id", frame.id}};
    if(frame.state == "queued" && Clock::now() >= frame.deadline) {
        pending = false;
        frame.state = "failed";
        frame.code = "capture_timeout";
    }
    if(frame.state == "completed" && !frame.published) {
        // Publish on the RPC thread. A full disk must report an IO failure,
        // never SIGBUS in the game from a file-backed GL destination.
        const auto bytes = size_t(frame.width) * frame.height * 4;
        size_t offset = 0;
        while(offset < bytes) {
            auto count = write(frame.fd, static_cast<char*>(frame.pixels) + offset, bytes - offset);
            if(count < 0 && errno == EINTR) continue;
            if(count <= 0) {
                frame.state = "failed";
                frame.code = "capture_io_error";
                break;
            }
            offset += size_t(count);
        }
        frame.published = true;
    }
    Json reply{{"protocol", 1}, {"request_id", frame.id}, {"state", frame.state}};
    if(frame.state == "failed") reply["code"] = frame.code;
    if(frame.state == "completed") {
        reply.update({{"width", frame.width}, {"height", frame.height},
                      {"source_time_ns", frame.timestamp}});
    }
    return reply;
}
}

void initialize(const std::string& path) { directory = path; }

Json request(const Json& input) {
    if(!input.is_object() || input.size() != 2 || !input.contains("op") || !input.contains("request_id") ||
       !input["op"].is_string() || !input["request_id"].is_string())
        return failure("invalid_request");
    const auto id = input.at("request_id").get<std::string>();
    const auto op = input.at("op").get<std::string>();
    if(!std::regex_match(id, std::regex("[0-9a-f]{32}")) ||
       (op != "capture" && op != "poll" && op != "release")) return failure("invalid_request");
    std::lock_guard<std::mutex> lock(mutex);
    if(op == "release") {
        if(current && current->id == id) {
            current->released = true;
            pending = false;
            // The render thread holds a shared reference until GL has finished.
            if(current->state != "capturing") current.reset();
        }
        return {{"state", "released"}, {"request_id", id}};
    }
    if(current && current->id == id) return result(*current);
    if(op == "poll") return failure("capture_not_found");
    if(directory.empty() || DeveloperLifecycle::exitRequested()) return failure("session_stopping");
    if(current) {
        result(*current);
        if(current->state == "capturing" ||
           (!current->released && Clock::now() < current->expires)) return failure("busy");
        current.reset();
    }
    auto frame = std::make_shared<Frame>();
    frame->id = id;
    const auto path = directory + "/screenshot-" + id + ".rgba";
    frame->fd = open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if(frame->fd < 0) return failure("capture_io_error");
    frame->path = path;
    // Reserve bounded, lazily committed RAM on the RPC thread. The renderer
    // only reads pixels; it never allocates files, encodes or waits on disk IO.
    frame->pixels = mmap(nullptr, maxBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if(frame->pixels == MAP_FAILED) return failure("capture_io_error");
    current = frame;
    pending = true;
    return result(*frame);
}

void capture(GameWindow& window) {
    if(!pending.load(std::memory_order_relaxed)) return;
    std::shared_ptr<Frame> frame;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if(!current || result(*current)["state"] != "queued") return;
        pending = false;
        frame = current;
        frame->state = "capturing";
    }
    int width, height;
    window.getWindowSize(width, height);
    height -= Settings::menubarsize.load();
    std::string error;
    if(width <= 0 || height <= 0 || size_t(width) * height > maxBytes / 4) {
        error = "capture_size_limit";
    } else if(!glReadPixels || !glBindFramebuffer || !glBindBuffer || !glPixelStorei || !glGetIntegerv || !glGetError) {
        error = "capture_unavailable";
    } else {
        // Error flags are sticky: errors left by the game are not readback failures.
        for(int i = 0; i < 16 && glGetError() != GL_NO_ERROR; ++i) {}
        GLint fbo = 0, pbo = 0, alignment = 4, rowLength = 0, skipRows = 0, skipPixels = 0;
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &fbo);
        glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pbo);
        glGetIntegerv(GL_PACK_ALIGNMENT, &alignment);
        glGetIntegerv(GL_PACK_ROW_LENGTH, &rowLength);
        glGetIntegerv(GL_PACK_SKIP_ROWS, &skipRows);
        glGetIntegerv(GL_PACK_SKIP_PIXELS, &skipPixels);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        glPixelStorei(GL_PACK_SKIP_ROWS, 0);
        glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, frame->pixels);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo);
        glPixelStorei(GL_PACK_ALIGNMENT, alignment);
        glPixelStorei(GL_PACK_ROW_LENGTH, rowLength);
        glPixelStorei(GL_PACK_SKIP_ROWS, skipRows);
        glPixelStorei(GL_PACK_SKIP_PIXELS, skipPixels);
        if(glGetError() != GL_NO_ERROR) error = "capture_graphics_error";
    }
    std::lock_guard<std::mutex> lock(mutex);
    frame->width = width;
    frame->height = height;
    frame->timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
    if(Clock::now() >= frame->deadline) error = "capture_timeout";
    frame->state = error.empty() ? "completed" : "failed";
    frame->code = error;
    if(frame->released && current == frame) current.reset();
}

void cleanup() {
    std::lock_guard<std::mutex> lock(mutex);
    pending = false;
    if(current) current->released = true;
    current.reset();
    directory.clear();
}
}
