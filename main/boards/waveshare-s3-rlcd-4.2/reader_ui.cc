// 阅读器页 UI —— TXT 电子书阅读
//
// 400×300 黑白单色 RLCD，全屏黑底。
// 中文统一使用小智自带 font_puhui_16_4（7415 常用汉字），避免缺字。
//
// 布局：
// ┌──────────────────────────────────────────┐
// │《书名》 第3/120章 42%   [WiFi][电池][85%]│ 顶行：书信息 + 状态胶囊
// │  ┌────────────────────────────────────┐  │
// │  │ 正文（白底卡片，按页渲染）            │  │ 主体：约 22 字 × 9 行
// │  │                                    │  │
// │  └────────────────────────────────────┘  │
// │  ┌────────────────────────────────────┐  │
// │  │[emoji] 待命 │  AI 待命              │  │ 底部 AI 状态卡（与三页统一）
// │  └────────────────────────────────────┘  │
// └──────────────────────────────────────────┘
//
// 交互：
// - USER 单击：下一页（章末自动进下一章）
// - USER 双击：上一章    USER 长按：下一章
// - 语音：self.disp.switch mode=reader
// - 书籍放在 SD 卡 /sdcard/books/*.txt（GBK 自动转码缓存）

#include "custom_lcd_display.h"
#include "managers/reader_manager.h"
#include "settings.h"

#include <esp_log.h>
#include <cstring>
#include <cstdio>

// 字体
LV_FONT_DECLARE(alibaba_puhui_16);   // 纯数字/ASCII（电量百分比）
LV_FONT_DECLARE(font_puhui_16_4);    // 16px 小智完整字库（正文/中文）
LV_FONT_DECLARE(font_puhui_14_1);    // 14px 小字（顶行书信息）

// 状态栏图标
LV_IMAGE_DECLARE(ui_img_wifi);
LV_IMAGE_DECLARE(ui_img_wifi_off);
LV_IMAGE_DECLARE(ui_img_battery_full);

static const char *TAG = "ReaderUI";

// 正文卡片几何（与布局图对应）
static const int SCR_W = 400;
static const int SCR_H = 300;
static const int PAD = 12;
static const int CARD_X = PAD;
static const int CARD_Y = 36;
static const int CARD_W = SCR_W - PAD * 2;      // 376
static const int CARD_H = 180;                   // y 36..216
static const int CARD_BORDER = 2;
static const int CARD_PAD = 10;
// 正文可用高度：卡片高 - 上下边框 - 上下内边距
static const uint32_t CONTENT_MAX_H = CARD_H - CARD_BORDER * 2 - CARD_PAD * 2;  // 156px
static const int32_t CONTENT_W = CARD_W - CARD_BORDER * 2 - CARD_PAD * 2;       // 352px

void CustomLcdDisplay::SetupReaderUI() {
    DisplayLockGuard lock(this);

    lv_obj_t *root = lv_screen_active();
    const lv_font_t *font_num  = &alibaba_puhui_16;
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
    // 第 1 层：顶行 —— 左侧书信息 + 右侧状态胶囊
    // ============================================================

    reader_top_label_ = lv_label_create(page);
    lv_obj_set_style_text_font(reader_top_label_, font_sm, 0);
    lv_obj_set_style_text_color(reader_top_label_, lv_color_white(), 0);
    lv_obj_set_width(reader_top_label_, 250);
    lv_label_set_long_mode(reader_top_label_, LV_LABEL_LONG_DOT);
    lv_obj_align(reader_top_label_, LV_ALIGN_TOP_LEFT, 10, 10);
    lv_label_set_text(reader_top_label_, "电子书");

    // 右上角状态栏胶囊（与音乐页一致）
    lv_obj_t *status_bar = lv_obj_create(page);
    lv_obj_set_size(status_bar, 115, 28);
    lv_obj_set_style_bg_opa(status_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(status_bar, lv_color_white(), 0);
    lv_obj_set_style_border_width(status_bar, 0, 0);
    lv_obj_set_style_radius(status_bar, 14, 0);
    lv_obj_align(status_bar, LV_ALIGN_TOP_RIGHT, -8, 4);
    lv_obj_set_style_pad_left(status_bar, 8, 0);
    lv_obj_set_style_pad_right(status_bar, 8, 0);
    lv_obj_set_style_pad_column(status_bar, 5, 0);
    lv_obj_set_style_pad_row(status_bar, 0, 0);
    lv_obj_remove_flag(status_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(status_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status_bar, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    reader_wifi_icon_img_ = lv_image_create(status_bar);
    lv_image_set_src(reader_wifi_icon_img_, &ui_img_wifi_off);
    reader_battery_icon_img_ = lv_image_create(status_bar);
    lv_image_set_src(reader_battery_icon_img_, &ui_img_battery_full);
    reader_battery_pct_label_ = lv_label_create(status_bar);
    lv_obj_set_style_text_font(reader_battery_pct_label_, font_num, 0);
    lv_obj_set_style_text_color(reader_battery_pct_label_, lv_color_black(), 0);
    lv_label_set_text(reader_battery_pct_label_, "---%");

    // ============================================================
    // 第 2 层：正文白底卡片
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

    const int text_w = CONTENT_W;

    reader_content_label_ = lv_label_create(reader_content_card_);
    lv_obj_set_style_text_font(reader_content_label_, font_cn, 0);
    lv_obj_set_style_text_color(reader_content_label_, lv_color_black(), 0);
    lv_obj_set_style_text_align(reader_content_label_, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_width(reader_content_label_, text_w);
    lv_obj_set_style_text_line_space(reader_content_label_, 3, 0);
    lv_label_set_long_mode(reader_content_label_, LV_LABEL_LONG_WRAP);
    lv_obj_align(reader_content_label_, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_label_set_text(reader_content_label_,
        "电子书\n\n"
        "把 .txt 文件放到 SD 卡:\n"
        "/sdcard/books/\n\n"
        "支持 UTF-8 和 GBK 编码\n"
        "USER 键翻页 · 双击上一章 · 长按下一章");

    // ============================================================
    // 第 3 层：底部 AI 状态卡（与音乐页一致）
    // ============================================================

    const int ai_h = 72;
    const int ai_w = SCR_W - PAD * 2;
    const int ai_y = SCR_H - ai_h - 6;
    const int emotion_w = 56;

    lv_obj_t *ai_card = lv_obj_create(page);
    lv_obj_set_size(ai_card, ai_w, ai_h);
    lv_obj_set_pos(ai_card, PAD, ai_y);
    lv_obj_set_style_bg_color(ai_card, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(ai_card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ai_card, 2, 0);
    lv_obj_set_style_border_color(ai_card, lv_color_black(), 0);
    lv_obj_set_style_radius(ai_card, 16, 0);
    lv_obj_set_style_pad_all(ai_card, 0, 0);
    lv_obj_remove_flag(ai_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_clip_corner(ai_card, true, 0);

    reader_emotion_img_ = lv_image_create(ai_card);
    lv_obj_set_size(reader_emotion_img_, 40, 40);
    lv_image_set_inner_align(reader_emotion_img_, LV_IMAGE_ALIGN_CENTER);
    lv_obj_align(reader_emotion_img_, LV_ALIGN_LEFT_MID, 10, -10);
    lv_obj_add_flag(reader_emotion_img_, LV_OBJ_FLAG_HIDDEN);

    reader_emotion_label_ = lv_label_create(ai_card);
    lv_obj_set_style_text_font(reader_emotion_label_, font_cn, 0);
    lv_obj_set_style_text_color(reader_emotion_label_, lv_color_black(), 0);
    lv_obj_set_style_text_align(reader_emotion_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(reader_emotion_label_, emotion_w);
    lv_label_set_long_mode(reader_emotion_label_, LV_LABEL_LONG_WRAP);
    lv_label_set_text(reader_emotion_label_, "待命");
    lv_obj_align(reader_emotion_label_, LV_ALIGN_LEFT_MID, 4, 20);

    lv_obj_t *divider = lv_obj_create(ai_card);
    lv_obj_set_size(divider, 2, ai_h - 20);
    lv_obj_set_style_bg_color(divider, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(divider, 0, 0);
    lv_obj_set_style_radius(divider, 1, 0);
    lv_obj_align(divider, LV_ALIGN_LEFT_MID, emotion_w + 10, 0);
    lv_obj_remove_flag(divider, LV_OBJ_FLAG_SCROLLABLE);

    const int text_x = emotion_w + 18;
    const int ai_text_w = ai_w - text_x - 12;
    reader_chat_status_label_ = lv_label_create(ai_card);
    lv_obj_set_style_text_font(reader_chat_status_label_, font_cn, 0);
    lv_obj_set_style_text_color(reader_chat_status_label_, lv_color_black(), 0);
    lv_obj_set_style_text_align(reader_chat_status_label_, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_width(reader_chat_status_label_, ai_text_w);
    lv_obj_set_style_text_line_space(reader_chat_status_label_, 3, 0);
    lv_label_set_long_mode(reader_chat_status_label_, LV_LABEL_LONG_WRAP);
    lv_label_set_text(reader_chat_status_label_, "AI 待命");
    lv_obj_align(reader_chat_status_label_, LV_ALIGN_LEFT_MID, text_x, 0);

    ESP_LOGI(TAG, "阅读页 UI 创建完成");
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
            lv_label_set_text(reader_content_label_,
                "未找到书籍\n\n"
                "把 .txt 文件放到 SD 卡:\n"
                "/sdcard/books/\n\n"
                "支持 UTF-8 和 GBK 编码");
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
