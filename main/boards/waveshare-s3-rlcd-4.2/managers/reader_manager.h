#ifndef __READER_MANAGER_H__
#define __READER_MANAGER_H__

#include <string>
#include <vector>
#include <functional>
#include <cstdint>

// 电子书管理器（单例）
//
// 职责：
// - 扫描 /sdcard/books/*.txt
// - 编码检测（严格 UTF-8 校验），GBK 自动转码并缓存为 /sdcard/books/.cache/*.utf8
// - 章节索引（第X章/回/节、楔子/序章等；无章节标记时按 ~24KB 行边界分块）
// - 分页：由显示层提供测量回调（LVGL 实测文本高度），页偏移存于内存
// - 阅读进度持久化（NVS: Settings("reader")）
//
// 本类只做文件与文本处理，不直接操作 LVGL。
class ReaderManager {
public:
    // 测量回调：返回 [begin, end) 文本在正文区（固定宽度、指定字体）的渲染高度（px）
    using MeasureFn = std::function<uint32_t(const char* begin, const char* end)>;

    static ReaderManager& getInstance() {
        static ReaderManager instance;
        return instance;
    }

    // 扫描书目（返回纯文件名列表，如 "三体.txt"）
    std::vector<std::string> scanBooks();

    // 打开书籍：编码处理 -> UTF-8 缓存，建立章节索引，恢复上次阅读进度
    // 成功后 chapter_text_ 尚未加载，需调用 loadChapter()
    bool openBook(const std::string& filename);
    bool isOpen() const { return open_; }

    const std::string& bookFile() const { return book_file_; }
    const std::string& bookTitle() const { return book_title_; }

    int chapterCount() const { return (int)chapters_.size(); }
    int currentChapter() const { return chapter_; }
    const std::string& chapterTitle(int idx) const;

    // 读取指定章节全文到内存（越界自动钳位）。返回章节文本
    bool loadChapter(int idx);
    const std::string& chapterText() const { return chapter_text_; }

    // 对当前章节文本分页（调用前需 loadChapter；measure 由显示层提供）
    void paginate(const MeasureFn& measure, uint32_t max_height);

    int pageCount() const { return (int)page_offsets_.size(); }
    int currentPage() const { return page_; }
    void setPage(int p);

    // 当前页文本视图（指向 chapter_text_ 内部，不拷贝）
    const char* pageBegin() const;
    const char* pageEnd() const;

    // 全书进度 0..100（章节 + 页 估算）
    int progressPercent() const;

    // 打开书籍时从 NVS 恢复的页码（分页完成后由显示层应用一次），-1 表示无
    int restorePage() const { return restore_page_; }
    void clearRestorePage() { restore_page_ = -1; }

    // 保存/恢复进度（NVS）
    void saveProgress();

private:
    ReaderManager() = default;

    struct Chapter {
        size_t offset;      // 在 UTF-8 文件中的字节偏移
        std::string title;  // 章节标题（显示用，已截断）
    };

    // 确保文件是 UTF-8：返回可直接读取的路径（原路径或 .cache 缓存路径）
    std::string ensureUtf8(const std::string& src_path, const std::string& cache_key);

    // 在 UTF-8 文件上建立章节索引；无标记时按固定大小行边界分块
    bool buildChapterIndex(const std::string& utf8_path);

    static constexpr const char* BOOKS_DIR = "/sdcard/books";
    static constexpr const char* CACHE_DIR = "/sdcard/books/.cache";
    static constexpr size_t MAX_CHAPTER_BYTES = 2 * 1024 * 1024;   // 单章硬上限
    static constexpr size_t NO_MARKER_BLOCK = 24 * 1024;           // 无章节标记时的分块大小

    bool open_ = false;
    std::string book_file_;    // 原始文件名（不含路径）
    std::string book_title_;   // 无扩展名标题
    std::string utf8_path_;    // 实际读取的 UTF-8 文件路径

    std::vector<Chapter> chapters_;
    int chapter_ = 0;
    std::string chapter_text_;          // 当前章节全文（UTF-8）

    std::vector<size_t> page_offsets_;  // 每页起始偏移（相对 chapter_text_）
    int page_ = 0;
    int restore_page_ = -1;
};

#endif
