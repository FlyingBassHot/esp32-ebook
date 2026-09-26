// 阅读器页 UI —— TXT 电子书阅读
//
// 400×300 黑白单色 RLCD，全屏黑底。**无顶栏**（沉浸阅读）。
// 中文统一使用小智自带 font_puhui_16_4（7415 常用汉字），避免缺字。
//
// 布局：
// ┌──────────────────────────────────────────┐
// │  ┌────────────────────────────────────┐  │
// │  │《书名》 第3/120章 42%              │  │  通栏卡片 384×284，8px 对称边距
// │  │ ───────────────────────────────    │  │  卡内首行：书信息（黑字居中）
// │  │ 正文（白底，按页渲染）               │  │  主体：约 23 字 × 11 行
// │  │                                    │  │
// │  └────────────────────────────────────┘  │
// └──────────────────────────────────────────┘
//
// 交互：
// - USER 单击：下一页（章末自动进下一章）
// - USER 长按：下一章    BOOT 双击：退出（切页）
// - 语音：self.disp.switch mode=reader
// - 书籍放在 SD 卡 /sdcard/books/*.txt（GBK 自动转码缓存）

#include "custom_lcd_display.h"
#include "managers/reader_manager.h"
#include "settings.h"

#include <esp_log.h>
#include <cstring>
#include <cstdio>

// 字体
LV_FONT_DECLARE(alibaba_puhui_16);   // 纯数字/ASCII
LV_FONT_DECLARE(font_puhui_16_4);    // 16px 小智完整字库（正文/中文）
LV_FONT_DECLARE(font_puhui_14_1);    // 14px 小字（卡内书信息）

static const char *TAG = "ReaderUI";

// 正文卡片几何（通栏，对称 8px 边距）
static const int SCR_W = 400;
static const int SCR_H = 300;
static const int CARD_X = 8;
static const int CARD_Y = 8;
static const int CARD_W = 384;                  // 8..392
static const int CARD_H = 284;                   // 8..292
static const int CARD_BORDER = 2;
static const int CARD_PAD = 10;
static const int HEADER_H = 32;                  // 卡内首行书信息 + 分隔线占位
// 正文可用高度：卡片高 - 上下边框 - 上下内边距 - 首行高度
static const uint32_t CONTENT_MAX_H = CARD_H - CARD_BORDER * 2 - CARD_PAD * 2 - HEADER_H;  // 228px
static const int32_t CONTENT_W = CARD_W - CARD_BORDER * 2 - CARD_PAD * 2;                  // 360px

void CustomLcdDisplay::SetupReaderUI() {
    DisplayLockGuard lock(this);

    lv_obj_t *root = lv_screen_active();
    const lv_font_t *font_cn   = &font_puhui_16_4;
    const lv_font_t *font_sm   = &font_puhui_14_1;

    // ===== 阅读页容器（全屏黑底，初始隐藏）=====
    reader_page_ = lv_obj_create(root);
    lv_obj_set_size(reader_page_, SCR_W, SCR_H);
    lv_obj_set_pos(reader_page_, 0, 0);
    lv_obj_set_style_bg_color(reader_page_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(reader_page_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(reader_page_, 0, 0);
    lv_obj_set_style_pad_all(reader_page_, 0, 0);
    lv_obj_set_style_radius(reader_page_, 0, 0);
    lv_obj_remove_flag(reader_page_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(reader_page_, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *page = reader_page_;

    // ============================================================
    // 通栏正文卡片（无顶栏：小智/胶囊/时间/温湿度全部移除）
    // ============================================================

    reader_content_card_ = lv_obj_create(page);
    lv_obj_set_size(reader_content_card_, CARD_W, CARD_H);
    lv_obj_set_pos(reader_content_card_, CARD_X, CARD_Y);
    lv_obj_set_style_bg_color(reader_content_card_, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(reader_content_card_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(reader_content_card_, CARD_BORDER, 0);
    lv_obj_set_style_border_color(reader_content_card_, lv_color_black(), 0);
    lv_obj_set_style_radius(reader_content_card_, 16, 0);
    lv_obj_set_style_pad_all(reader_content_card_, CARD_PAD, 0);
    lv_obj_remove_flag(reader_content_card_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_clip_corner(reader_content_card_, true, 0);

    // 卡内首行：书信息（黑字居中）
    reader_top_label_ = lv_label_create(reader_content_card_);
    lv_obj_set_style_text_font(reader_top_label_, font_sm, 0);
    lv_obj_set_style_text_color(reader_top_label_, lv_color_black(), 0);
    lv_obj_set_style_text_align(reader_top_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(reader_top_label_, CONTENT_W);
    lv_label_set_long_mode(reader_top_label_, LV_LABEL_LONG_DOT);
    lv_obj_align(reader_top_label_, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_label_set_text(reader_top_label_, "电子书");

    // 首行下分隔线
    lv_obj_t *sep = lv_obj_create(reader_content_card_);
    lv_obj_set_size(sep, CONTENT_W, 1);
    lv_obj_set_pos(sep, 0, HEADER_H - 8);
    lv_obj_set_style_bg_color(sep, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_30, 0);
    lv_obj_set_style_border_width(sep, 0, 0);
    lv_obj_remove_flag(sep, LV_OBJ_FLAG_SCROLLABLE);

    // 正文
    reader_content_label_ = lv_label_create(reader_content_card_);
    lv_obj_set_style_text_font(reader_content_label_, font_cn, 0);
    lv_obj_set_style_text_color(reader_content_label_, lv_color_black(), 0);
    lv_obj_set_style_text_align(reader_content_label_, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_width(reader_content_label_, CONTENT_W);
    lv_obj_set_style_text_line_space(reader_content_label_, 3, 0);
    lv_label_set_long_mode(reader_content_label_, LV_LABEL_LONG_WRAP);
    lv_obj_align(reader_content_label_, LV_ALIGN_TOP_LEFT, 0, HEADER_H);
    lv_label_set_text(reader_content_label_, "SD 卡未检测到书籍");

    ESP_LOGI(TAG, "阅读页 UI 创建完成（通栏卡 %dx%d，正文区 %dx%d）",
             CARD_W, CARD_H, (int)CONTENT_W, (int)CONTENT_MAX_H);
}

// ===== 阅读器加载与渲染（以下方法调用者需已持有 DisplayLock）=====

void CustomLcdDisplay::ReaderEnsureLoaded() {
    auto& mgr = ReaderManager::getInstance();
    if (mgr.isOpen()) return;

    // 懒加载：SD 卡在板级构造时已挂载
    auto books = mgr.scanBooks();
    if (books.empty()) {
        ESP_LOGW(TAG, "SD 卡上未找到书籍（/sdcard/books/*.txt）");
        if (reader_content_label_) {
            lv_label_set_text(reader_content_label_, "SD 卡未检测到书籍");
        }
        if (reader_top_label_) lv_label_set_text(reader_top_label_, "电子书");
        return;
    }

    // 优先打开上次读的书（openBook 内部会校验 NVS 中的 book 字段）
    std::string target = books[0];
    {
        Settings rd("reader", false);
        std::string last = rd.GetString("book", "");
        if (!last.empty()) {
            for (auto& b : books) {
                if (b == last) { target = b; break; }
            }
        }
    }

    if (!mgr.openBook(target)) {
        ESP_LOGE(TAG, "打开书籍失败: %s", target.c_str());
        return;
    }
    ReaderLoadChapter(mgr.currentChapter());
}

void CustomLcdDisplay::ReaderLoadChapter(int idx) {
    auto& mgr = ReaderManager::getInstance();
    if (!mgr.isOpen()) return;

    mgr.loadChapter(idx);

    // 分页测量：与正文 label 等价的纯文本测量（同字体/宽度/行距，不依赖控件布局）
    auto measure = [](const char* b, const char* e) -> uint32_t {
        if (e <= b) return 1;
        std::string tmp(b, e - b);
        lv_point_t sz;
        lv_text_get_size(&sz, tmp.c_str(), &font_puhui_16_4,
                         /*letter_space=*/0, /*line_space=*/3,
                         CONTENT_W, LV_TEXT_FLAG_NONE);
        return sz.y > 0 ? (uint32_t)sz.y : 1;
    };
    mgr.paginate(measure, CONTENT_MAX_H);

    // 应用打开书籍时恢复的页码
    if (mgr.restorePage() >= 0) {
        mgr.setPage(mgr.restorePage());
        mgr.clearRestorePage();
    }
}

void CustomLcdDisplay::ReaderRenderPage() {
    auto& mgr = ReaderManager::getInstance();
    if (!mgr.isOpen() || !reader_content_label_) return;

    std::string page(mgr.pageBegin(), mgr.pageEnd());
    lv_label_set_text(reader_content_label_, page.c_str());

    if (reader_top_label_) {
        char buf[160];
        snprintf(buf, sizeof(buf), "《%s》 第%d/%d章 %d%%",
                 mgr.bookTitle().c_str(),
                 mgr.currentChapter() + 1, mgr.chapterCount(),
                 mgr.progressPercent());
        lv_label_set_text(reader_top_label_, buf);
    }

    mgr.saveProgress();
    ESP_LOGD(TAG, "渲染第 %d/%d 页 (章 %d)", mgr.currentPage() + 1,
             mgr.pageCount(), mgr.currentChapter() + 1);
}

// ===== 阅读器公开操作（自动加锁）=====

void CustomLcdDisplay::SwitchToReaderPage() {
    DisplayLockGuard lock(this);
    if (display_mode_ != MODE_READER) {
        display_mode_ = MODE_READER;
        ApplyDisplayMode();   // 内部会触发 ReaderEnsureLoaded
        ESP_LOGI(TAG, "自动切换到阅读页");
    }
}

void CustomLcdDisplay::ReaderNextPage() {
    DisplayLockGuard lock(this);
    ReaderEnsureLoaded();
    auto& mgr = ReaderManager::getInstance();
    if (!mgr.isOpen()) return;

    if (mgr.currentPage() + 1 < mgr.pageCount()) {
        mgr.setPage(mgr.currentPage() + 1);
    } else if (mgr.currentChapter() + 1 < mgr.chapterCount()) {
        ReaderLoadChapter(mgr.currentChapter() + 1);
    } else {
        ESP_LOGI(TAG, "已是全书最后一页");
        return;
    }
    ReaderRenderPage();
}

void CustomLcdDisplay::ReaderPrevPage() {
    DisplayLockGuard lock(this);
    ReaderEnsureLoaded();
    auto& mgr = ReaderManager::getInstance();
    if (!mgr.isOpen()) return;

    if (mgr.currentPage() > 0) {
        mgr.setPage(mgr.currentPage() - 1);
    } else if (mgr.currentChapter() > 0) {
        ReaderLoadChapter(mgr.currentChapter() - 1);
        mgr.setPage(mgr.pageCount() - 1);   // 上一章末页
    } else {
        ESP_LOGI(TAG, "已是全书第一页");
        return;
    }
    ReaderRenderPage();
}

void CustomLcdDisplay::ReaderNextChapter() {
    DisplayLockGuard lock(this);
    ReaderEnsureLoaded();
    auto& mgr = ReaderManager::getInstance();
    if (!mgr.isOpen()) return;
    if (mgr.currentChapter() + 1 >= mgr.chapterCount()) {
        ESP_LOGI(TAG, "已是最后一章");
        return;
    }
    ReaderLoadChapter(mgr.currentChapter() + 1);
    ReaderRenderPage();
}

void CustomLcdDisplay::ReaderPrevChapter() {
    DisplayLockGuard lock(this);
    ReaderEnsureLoaded();
    auto& mgr = ReaderManager::getInstance();
    if (!mgr.isOpen()) return;
    if (mgr.currentChapter() <= 0) {
        ESP_LOGI(TAG, "已是第一章");
        return;
    }
    ReaderLoadChapter(mgr.currentChapter() - 1);
    ReaderRenderPage();
}

bool CustomLcdDisplay::ReaderOpenBook(const std::string& filename) {
    DisplayLockGuard lock(this);
    auto& mgr = ReaderManager::getInstance();
    if (!mgr.openBook(filename)) return false;
    ReaderLoadChapter(mgr.currentChapter());
    if (display_mode_ != MODE_READER) {
        display_mode_ = MODE_READER;
        ApplyDisplayMode();
    }
    ReaderRenderPage();
    return true;
}

std::vector<std::string> CustomLcdDisplay::ReaderListBooks() {
    return ReaderManager::getInstance().scanBooks();
}
