// 电子书管理器实现
//
// 文件/编码/章节/分页/进度，不含任何 LVGL 调用（测量通过回调注入）。

#include "reader_manager.h"
#include "gbk_table.h"

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <algorithm>

#include <esp_log.h>

#include "settings.h"

static const char* TAG = "ReaderMgr";

// ===== UTF-8 工具 =====

extern "C" int unicode_to_utf8(uint32_t cp, char* buf) {
    if (cp < 0x80) {
        buf[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        buf[0] = (char)(0xC0 | (cp >> 6));
        buf[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        buf[0] = (char)(0xE0 | (cp >> 12));
        buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    buf[0] = (char)(0xF0 | (cp >> 18));
    buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    buf[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

// 严格 UTF-8 校验。buf_end_incomplete=true 时允许最后一个序列被缓冲区截断
static bool validateUtf8(const uint8_t* s, size_t len, bool allow_tail_cut) {
    size_t i = 0;
    while (i < len) {
        uint8_t c = s[i];
        if (c < 0x80) { i++; continue; }

        int extra;
        uint32_t cp;
        if ((c & 0xE0) == 0xC0)      { extra = 1; cp = c & 0x1F; if (c < 0xC2) return false; }
        else if ((c & 0xF0) == 0xE0) { extra = 2; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { extra = 3; cp = c & 0x07; if (c > 0xF4) return false; }
        else return false;

        if (i + extra >= len) {
            // 序列在缓冲区末尾被截断
            if (!allow_tail_cut) return false;
            // 已有部分必须全是合法续字节
            for (size_t k = i + 1; k < len; k++) {
                if ((s[k] & 0xC0) != 0x80) return false;
            }
            return true;
        }
        for (int k = 1; k <= extra; k++) {
            uint8_t cc = s[i + k];
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (extra == 1 && cp < 0x80) return false;
        if (extra == 2 && cp < 0x800) return false;
        if (extra == 3 && cp < 0x10000) return false;
        if (cp > 0x10FFFF) return false;
        if (cp >= 0xD800 && cp <= 0xDFFF) return false;
        i += extra + 1;
    }
    return true;
}

static bool looksLikeUtf8(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    uint8_t buf[16384];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    if (n == 0) return true;  // 空文件按 UTF-8 处理
    // UTF-8 BOM
    if (n >= 3 && buf[0] == 0xEF && buf[1] == 0xBB && buf[2] == 0xBF) return true;
    return validateUtf8(buf, n, /*allow_tail_cut=*/true);
}

// ===== GBK -> UTF-8 流式转换 =====

static bool convertGbkToUtf8(const std::string& src, const std::string& dst) {
    FILE* fi = fopen(src.c_str(), "rb");
    if (!fi) return false;
    FILE* fo = fopen(dst.c_str(), "wb");
    if (!fo) { fclose(fi); return false; }

    uint8_t buf[8192];
    int pending_lead = -1;
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fi)) > 0) {
        for (size_t i = 0; i < n; i++) {
            uint8_t b = buf[i];
            if (pending_lead >= 0) {
                char u8[4];
                int len = 0;
                int ti = gbk_trail_index(b);
                uint16_t cp = 0;
                if (ti >= 0) cp = GBK_TABLE[pending_lead - 0x81][ti];
                if (cp != 0) {
                    len = unicode_to_utf8(cp, u8);
                } else {
                    u8[0] = '?'; len = 1;  // 无法映射的序列
                }
                fwrite(u8, 1, len, fo);
                pending_lead = -1;
            } else if (b < 0x80) {
                fputc(b, fo);
            } else if (b >= 0x81 && b <= 0xFE) {
                pending_lead = b;
            } else if (b == 0x80) {
                // CP936: 0x80 = 欧元符号
                fwrite("\xE2\x82\xAC", 1, 3, fo);
            } else {
                fputc('?', fo);
            }
        }
    }
    if (pending_lead >= 0) fputc('?', fo);

    fclose(fi);
    fclose(fo);
    return true;
}

// ===== 章节标记识别 =====

static std::string trimLine(const std::string& s) {
    size_t b = 0, e = s.size();
    // 跳过 UTF-8 BOM（文件首行可能带 BOM）
    if (b + 2 < s.size() && (uint8_t)s[b] == 0xEF &&
        (uint8_t)s[b + 1] == 0xBB && (uint8_t)s[b + 2] == 0xBF) b += 3;
    // 跳过开头的全宽空格 U+3000 = E3 80 80
    while (b + 2 < s.size() && (uint8_t)s[b] == 0xE3 &&
           (uint8_t)s[b + 1] == 0x80 && (uint8_t)s[b + 2] == 0x80) b += 3;
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r')) b++;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' ||
                     s[e - 1] == '\r' || s[e - 1] == '\n')) e--;
    return s.substr(b, e - b);
}

// 取下一个 UTF-8 码点（i 前进；非法字节返回 0xFFFD）
static uint32_t nextCp(const std::string& s, size_t& i) {
    if (i >= s.size()) return 0;
    uint8_t c = (uint8_t)s[i];
    uint32_t cp;
    int extra;
    if (c < 0x80) { cp = c; extra = 0; }
    else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
    else { i++; return 0xFFFD; }
    i++;
    for (int k = 1; k <= extra && i < s.size(); k++, i++) {
        cp = (cp << 6) | ((uint8_t)s[i] & 0x3F);
    }
    return cp;
}

// 章节号允许的码点：数字 0-9 + 中文数字/大写数字
static bool isChapterNumCp(uint32_t cp) {
    if (cp >= '0' && cp <= '9') return true;
    switch (cp) {
        case 0x96F6:  // 零
        case 0x3007:  // 〇
        case 0x4E00: case 0x4E8C: case 0x4E09: case 0x56DB:  // 一二三四
        case 0x4E94: case 0x516D: case 0x4E03: case 0x516B:  // 五六七八
        case 0x4E5D: case 0x5341: case 0x767E: case 0x5343:  // 九十百千
        case 0x4E07: case 0x4EBF: case 0x4E24:               // 万亿两
        case 0x58F9: case 0x8D30: case 0x53C1: case 0x8086:  // 壹贰叁肆
        case 0x4F0D: case 0x9646: case 0x67D2: case 0x634C:  // 伍陆柒捌
        case 0x7396: case 0x62FE: case 0x4F70: case 0x4EDF:  // 玖拾佰仟
            return true;
        default:
            return false;
    }
}

static const char* STANDALONE_TITLES[] = {
    "楔子", "序章", "序言", "前言", "引子", "后记", "尾声", "终章", "结局", "番外",
};

// 判断（已 trim 的）行是否为章节标题；是则写入 title
static bool matchChapterLine(const std::string& line, std::string* title) {
    if (line.empty() || line.size() > 80) return false;

    // 独立标题关键词（前缀匹配）
    for (const char* kw : STANDALONE_TITLES) {
        size_t klen = strlen(kw);
        if (line.size() >= klen && memcmp(line.data(), kw, klen) == 0) {
            *title = line.substr(0, std::min<size_t>(line.size(), 60));
            return true;
        }
    }

    // "第X章/回/节/卷"：第 + 1..8 个数字码点 + 章回节卷
    size_t i = 0;
    if (nextCp(line, i) != 0x7B2C) return false;   // 第

    int num = 0;
    while (i < line.size() && num < 8) {
        size_t before = i;
        uint32_t c = nextCp(line, i);
        if (!isChapterNumCp(c)) { i = before; break; }
        num++;
    }
    if (num == 0 || i >= line.size()) return false;

    uint32_t marker = nextCp(line, i);
    if (marker == 0x7AE0 || marker == 0x56DE ||   // 章 回
        marker == 0x8282 || marker == 0x5377) {   // 节 卷
        *title = line.substr(0, std::min<size_t>(line.size(), 60));
        return true;
    }
    return false;
}

// ===== 扫描书目 =====

std::vector<std::string> ReaderManager::scanBooks() {
    std::vector<std::string> result;

    // 目录不存在时尝试创建（SD 已挂载的前提下）
    mkdir(BOOKS_DIR, 0755);
    mkdir(CACHE_DIR, 0755);

    DIR* dir = opendir(BOOKS_DIR);
    if (!dir) {
        ESP_LOGW(TAG, "无法打开书目目录 %s: %s", BOOKS_DIR, strerror(errno));
        return result;
    }
    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        std::string name = ent->d_name;
        if (ent->d_type == DT_DIR) continue;
        if (name.size() > 4) {
            std::string ext = name.substr(name.size() - 4);
            for (auto& ch : ext) ch = (char)tolower((unsigned char)ch);
            if (ext == ".txt") result.push_back(name);
        }
    }
    closedir(dir);

    std::sort(result.begin(), result.end());
    ESP_LOGI(TAG, "扫描到 %u 本书", (unsigned)result.size());
    return result;
}

// ===== 编码处理 =====

std::string ReaderManager::ensureUtf8(const std::string& src_path, const std::string& cache_key) {
    if (looksLikeUtf8(src_path)) {
        return src_path;
    }

    mkdir(CACHE_DIR, 0755);
    std::string cache_path = std::string(CACHE_DIR) + "/" + cache_key + ".utf8";

    struct stat st_src{}, st_cache{};
    bool cache_ok = (stat(cache_path.c_str(), &st_cache) == 0);
    bool src_newer = true;
    if (cache_ok && stat(src_path.c_str(), &st_src) == 0) {
        src_newer = st_src.st_mtime > st_cache.st_mtime;
    }

    if (cache_ok && !src_newer) {
        ESP_LOGI(TAG, "复用转码缓存: %s", cache_path.c_str());
        return cache_path;
    }

    ESP_LOGI(TAG, "检测到非 UTF-8 编码（GBK），转码中: %s -> %s", src_path.c_str(), cache_path.c_str());
    std::string tmp_path = cache_path + ".tmp";
    if (!convertGbkToUtf8(src_path, tmp_path)) {
        ESP_LOGE(TAG, "GBK 转码失败");
        return src_path;  // 兜底：按原文件读（会乱码但不崩溃）
    }
    rename(tmp_path.c_str(), cache_path.c_str());
    return cache_path;
}

// ===== 章节索引 =====

bool ReaderManager::buildChapterIndex(const std::string& utf8_path) {
    chapters_.clear();

    FILE* f = fopen(utf8_path.c_str(), "rb");
    if (!f) {
        ESP_LOGE(TAG, "无法打开 %s", utf8_path.c_str());
        return false;
    }

    // 缓冲块读取（比 fgetc 快一个量级，缩短显示锁占用）
    uint8_t rbuf[4096];
    size_t rn = 0, ri = 0;
    auto nextByte = [&]() -> int {
        if (ri >= rn) {
            rn = fread(rbuf, 1, sizeof(rbuf), f);
            ri = 0;
            if (rn == 0) return EOF;
        }
        return rbuf[ri++];
    };

    // 第一遍：找章节标记
    size_t offset = 0;          // 当前行起始偏移
    size_t read_pos = 0;        // 已读字节
    std::string line;
    line.reserve(256);
    int c;
    bool in_line = false;
    auto flush_line = [&]() {
        if (!in_line) return;
        std::string t = trimLine(line);
        if (!t.empty()) {
            std::string title;
            if (matchChapterLine(t, &title)) {
                chapters_.push_back({offset, title});
            }
        }
        line.clear();
        offset = read_pos;  // 下一行起始
        in_line = false;
    };
    while ((c = nextByte()) != EOF) {
        read_pos++;
        in_line = true;
        if (c == '\n') {
            flush_line();
        } else if (line.size() < 256) {
            line.push_back((char)c);
        }
        // 超长行：只保留前 256 字节参与判断，但仍完整消费
    }
    flush_line();

    // 文件开头的 BOM 若被当作章节行的一部分不影响（BOM 行不会匹配章节）

    // 第二遍：无章节标记时按行边界分块
    if (chapters_.empty()) {
        ESP_LOGI(TAG, "未发现章节标记，按 %u 字节分块", (unsigned)NO_MARKER_BLOCK);
        rewind(f);
        rn = ri = 0;
        offset = 0;
        read_pos = 0;
        size_t last_split = 0;
        int block_no = 2;
        chapters_.push_back({0, "第1段"});   // 第一块从文件头开始
        line.clear();
        in_line = false;
        auto flush_split = [&]() {
            if (!in_line) return;
            if (read_pos - last_split >= NO_MARKER_BLOCK) {
                chapters_.push_back({offset, "第" + std::to_string(block_no++) + "段"});
                last_split = offset;
            }
            line.clear();
            offset = read_pos;
            in_line = false;
        };
        while ((c = nextByte()) != EOF) {
            read_pos++;
            in_line = true;
            if (c == '\n') flush_split();
        }
        flush_split();
    }

    fclose(f);

    if (chapters_.empty()) {
        chapters_.push_back({0, "全文"});
    }
    ESP_LOGI(TAG, "章节索引完成: %u 章", (unsigned)chapters_.size());
    return true;
}

// ===== 打开书籍 =====

bool ReaderManager::openBook(const std::string& filename) {
    std::string src_path = std::string(BOOKS_DIR) + "/" + filename;
    struct stat st;
    if (stat(src_path.c_str(), &st) != 0) {
        ESP_LOGE(TAG, "书籍不存在: %s", src_path.c_str());
        return false;
    }

    utf8_path_ = ensureUtf8(src_path, filename);
    if (!buildChapterIndex(utf8_path_)) {
        return false;
    }

    book_file_ = filename;
    size_t dot = filename.find_last_of('.');
    book_title_ = (dot == std::string::npos) ? filename : filename.substr(0, dot);

    // 恢复上次进度（页码在分页完成后由显示层通过 restorePage() 应用）
    chapter_ = 0;
    restore_page_ = -1;
    {
        Settings rd("reader", false);
        if (rd.GetString("book", "") == filename) {
            chapter_ = rd.GetInt("chapter", 0);
            restore_page_ = rd.GetInt("page", 0);
        }
    }
    if (chapter_ < 0) chapter_ = 0;
    if (chapter_ >= (int)chapters_.size()) chapter_ = (int)chapters_.size() - 1;

    chapter_text_.clear();
    page_offsets_.clear();
    page_offsets_.push_back(0);
    page_ = 0;

    open_ = true;
    ESP_LOGI(TAG, "打开书籍: %s (%u 章, 进度=章%d)", book_title_.c_str(),
             (unsigned)chapters_.size(), chapter_);
    return true;
}

const std::string& ReaderManager::chapterTitle(int idx) const {
    static const std::string empty;
    if (idx < 0 || idx >= (int)chapters_.size()) return empty;
    return chapters_[idx].title;
}

// ===== 章节读取 =====

bool ReaderManager::loadChapter(int idx) {
    if (!open_) return false;
    if (idx < 0) idx = 0;
    if (idx >= (int)chapters_.size()) idx = (int)chapters_.size() - 1;
    chapter_ = idx;

    size_t start = chapters_[idx].offset;
    size_t end = (idx + 1 < (int)chapters_.size())
                     ? chapters_[idx + 1].offset
                     : (size_t)-1;
    size_t want = (end == (size_t)-1) ? MAX_CHAPTER_BYTES
                                      : std::min(end - start, MAX_CHAPTER_BYTES);

    FILE* f = fopen(utf8_path_.c_str(), "rb");
    if (!f) {
        ESP_LOGE(TAG, "打开文件失败: %s", utf8_path_.c_str());
        return false;
    }
    fseek(f, (long)start, SEEK_SET);
    chapter_text_.resize(want);
    size_t got = fread(&chapter_text_[0], 1, want, f);
    fclose(f);
    chapter_text_.resize(got);

    // 跳过文件起始 BOM
    if (start == 0 && chapter_text_.size() >= 3 &&
        (uint8_t)chapter_text_[0] == 0xEF &&
        (uint8_t)chapter_text_[1] == 0xBB &&
        (uint8_t)chapter_text_[2] == 0xBF) {
        chapter_text_.erase(0, 3);
    }

    // 章节文本变化后页索引失效
    page_offsets_.clear();
    page_offsets_.push_back(0);
    page_ = 0;

    ESP_LOGD(TAG, "加载章节 %d/%d: %u 字节", chapter_ + 1, chapterCount(), (unsigned)got);
    return true;
}

// ===== 分页 =====

static size_t utf8Boundary(const std::string& s, size_t pos) {
    while (pos < s.size() && ((uint8_t)s[pos] & 0xC0) == 0x80) pos++;
    return pos;
}

void ReaderManager::paginate(const MeasureFn& measure, uint32_t max_height) {
    page_offsets_.clear();
    page_ = 0;
    if (chapter_text_.empty() || !measure || max_height == 0) {
        page_offsets_.push_back(0);
        return;
    }

    const size_t len = chapter_text_.size();
    size_t pos = 0;
    page_offsets_.push_back(0);

    while (pos < len) {
        // 初估一页约 400 个字符（16px 字体正文区约 22 字 × 9 行 ≈ 200，留余量）
        size_t guess = pos + 400;
        if (guess > len) guess = len;
        guess = utf8Boundary(chapter_text_, guess);

        uint32_t h = 0;
        int tries = 0;
        while (true) {
            h = measure(chapter_text_.c_str() + pos, chapter_text_.c_str() + guess);
            if (h <= max_height || guess - pos <= 64 || tries >= 5) break;
            // 按高度比例缩小（乘 0.92 安全系数）
            double ratio = (double)max_height / (double)h * 0.92;
            size_t span = (size_t)((guess - pos) * ratio);
            if (span < 64) span = 64;
            guess = pos + span;
            if (guess > len) guess = len;
            guess = utf8Boundary(chapter_text_, guess);
            tries++;
        }
        // 若还有余量，小幅增长（最多一次）
        if (h < max_height && guess < len && tries > 0) {
            size_t grow = (size_t)((guess - pos) * 0.06);
            size_t cand = utf8Boundary(chapter_text_, std::min(guess + grow, len));
            if (cand > guess &&
                measure(chapter_text_.c_str() + pos, chapter_text_.c_str() + cand) <= max_height) {
                guess = cand;
            }
        }

        if (guess <= pos) break;  // 防御：至少前进
        pos = guess;
        if (pos < len) page_offsets_.push_back(pos);
    }

    ESP_LOGI(TAG, "分页完成: %u 页", (unsigned)page_offsets_.size());
}

void ReaderManager::setPage(int p) {
    if (page_offsets_.empty()) page_offsets_.push_back(0);
    if (p < 0) p = 0;
    if (p >= (int)page_offsets_.size()) p = (int)page_offsets_.size() - 1;
    page_ = p;
}

const char* ReaderManager::pageBegin() const {
    if (chapter_text_.empty() || page_offsets_.empty()) return "";
    return chapter_text_.c_str() + page_offsets_[page_];
}

const char* ReaderManager::pageEnd() const {
    if (chapter_text_.empty() || page_offsets_.empty()) return "";
    size_t end = (page_ + 1 < (int)page_offsets_.size())
                     ? page_offsets_[page_ + 1]
                     : chapter_text_.size();
    return chapter_text_.c_str() + end;
}

int ReaderManager::progressPercent() const {
    int total = chapterCount();
    if (total <= 0) return 0;
    int in_chapter = 100;
    if (pageCount() > 1) {
        in_chapter = page_ * 100 / (pageCount() - 1);
    }
    return (chapter_ * 100 + in_chapter) / total;
}

void ReaderManager::saveProgress() {
    if (!open_) return;
    Settings wr("reader", true);
    wr.SetString("book", book_file_);
    wr.SetInt("chapter", chapter_);
    wr.SetInt("page", page_);
}
