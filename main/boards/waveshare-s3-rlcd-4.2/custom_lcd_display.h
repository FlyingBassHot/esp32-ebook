#ifndef __CUSTOM_LCD_DISPLAY_H__
#define __CUSTOM_LCD_DISPLAY_H__

#include <atomic>
#include <string>
#include <vector>
#include <driver/gpio.h>
#include "lcd_display.h"
#include "rlcd_driver.h"
#include "managers/sensor_manager.h"
#include "managers/weather_manager.h"

// 天气站 + 阅读器 + 系统信息 混合显示（改版：AI 对话卡全部移除）
//
// 屏幕布局 (400x300, 1-bit 单色 RLCD)，五页循环：
//   天气页   → 顶栏(温湿度 + 中央小智状态图标 + 状态胶囊)
//             上排：时钟卡 248×128 + 合并卡 130×128(日期/地点天气/备忘录居中2行)
//             下排：照片轮播(自适应宽,高122) + 书目列表(纯展示)
//   音乐页   → 顶栏(时间 + 中央小智 + 胶囊) / 唱片卡+歌曲卡 / 进度条(y262) / 音量胶囊(临时)
//   番茄钟页 → 状态 + 倒计时 + 进度条 + 设定信息
//   阅读页   → 无顶栏，通栏正文卡 384×284（卡内首行=书信息）
//   系统信息 → 白卡列表(CPU/运行/SRAM/PSRAM/电池/WiFi)
//
// 代码拆分为多个文件：
//   rlcd_driver.h/cc      - RLCD 硬件驱动层
//   weather_ui.cc          - 天气页布局（含照片轮播/书目列表）
//   music_ui.cc            - 音乐页布局
//   pomodoro_ui.cc         - 番茄钟布局
//   reader_ui.cc           - 阅读页布局
//   system_ui.cc           - 系统信息页布局
//   data_update_task.cc    - 后台数据更新任务
//   custom_lcd_display.cc  - 核心类
class CustomLcdDisplay : public LcdDisplay {
private:
    enum DisplayMode {
        MODE_WEATHER = 0,
        MODE_MUSIC = 1,
        MODE_POMODORO = 2,
        MODE_READER = 3,
        MODE_SYSTEM_INFO = 4,
    };
    DisplayMode display_mode_ = MODE_WEATHER;

    // RLCD 硬件驱动（独立模块，负责 SPI 通信和像素操作）
    RlcdDriver *rlcd_ = nullptr;
    lv_obj_t *weather_page_ = nullptr;
    lv_obj_t *music_page_ = nullptr;
    lv_obj_t *pomodoro_page_ = nullptr;
    lv_obj_t *system_page_ = nullptr;

    // ===== 顶栏中央「小智」状态标识（天气/音乐/番茄/系统 四页各一份）=====
    // 图标为代码绘制三态：0=待命(空心圆) 1=聆听(脉冲环) 2=说话(实心+声波)
    std::vector<lv_obj_t*> status_icons_;
    int chat_ui_state_ = 0;
    lv_timer_t *status_anim_timer_ = nullptr;

    // ===== 天气页 UI 组件 =====
    lv_obj_t *sensor_label_ = nullptr;      // 左上角温湿度标签
    lv_obj_t *time_label_ = nullptr;        // 大字时钟 "14:30"
    lv_obj_t *day_label_ = nullptr;         // 星期 "TUE"（合并卡左列）
    lv_obj_t *date_num_label_ = nullptr;    // 日期 "15"（合并卡左列）
    lv_obj_t *city_label_ = nullptr;        // 地点 "深圳市"（合并卡右列）
    lv_obj_t *weather_label_ = nullptr;     // 天气 "晴 25°C"（合并卡右列）

    // 备忘录（合并卡下部，居中 2 行轮播）
    lv_obj_t *memo_list_label_ = nullptr;
    std::vector<std::string> memo_lines_;
    int memo_view_idx_ = 0;
    lv_timer_t *memo_timer_ = nullptr;

    // 照片轮播（下排左侧，高122撑满、宽随图片比例，SD 卡 /sdcard/photos/*.png）
    lv_obj_t *photo_card_ = nullptr;
    lv_obj_t *photo_img_ = nullptr;
    std::vector<std::string> photo_files_;
    int photo_index_ = -1;
    int photo_card_w_ = 0;   // 当前照片卡宽度（随图片比例，上限 168）
    lv_timer_t *photo_timer_ = nullptr;

    // 书目列表（下排右侧，纯展示、居中、超量轮播）
    lv_obj_t *book_card_ = nullptr;
    lv_obj_t *book_sep_ = nullptr;
    lv_obj_t *book_title_label_ = nullptr;
    lv_obj_t *book_list_label_ = nullptr;
    std::vector<std::string> book_lines_;
    int book_view_idx_ = 0;
    lv_timer_t *book_timer_ = nullptr;

    // ===== 音乐页 UI 组件 =====
    lv_obj_t *music_title_label_ = nullptr;   // 歌名
    lv_obj_t *music_artist_label_ = nullptr;  // 歌手
    lv_obj_t *music_lyric_prev_label_ = nullptr;
    lv_obj_t *music_lyric_label_ = nullptr;
    lv_obj_t *music_lyric_next_label_ = nullptr;
    lv_obj_t *music_progress_bar_ = nullptr;
    lv_obj_t *music_progress_label_ = nullptr;
    lv_obj_t *music_time_label_ = nullptr;    // 左上角时钟
    lv_obj_t *music_wifi_icon_img_ = nullptr;
    lv_obj_t *music_battery_icon_img_ = nullptr;
    lv_obj_t *music_battery_pct_label_ = nullptr;

    // 音量临时胶囊（进度条与卡片之间，调节音量时显示 2 秒）
    lv_obj_t *music_volume_chip_ = nullptr;
    lv_obj_t *music_volume_label_ = nullptr;
    lv_timer_t *volume_hide_timer_ = nullptr;
    uint32_t volume_shown_until_ms_ = 0;

    // ===== 番茄钟 UI 组件 =====
    lv_obj_t *pomo_state_label_ = nullptr;
    lv_obj_t *pomo_countdown_label_ = nullptr;
    lv_obj_t *pomo_progress_bar_ = nullptr;
    lv_obj_t *pomo_info_label_ = nullptr;
    lv_obj_t *pomo_time_label_ = nullptr;
    lv_obj_t *pomo_sensor_label_ = nullptr;
    lv_obj_t *pomo_wifi_icon_img_ = nullptr;
    lv_obj_t *pomo_battery_icon_img_ = nullptr;
    lv_obj_t *pomo_battery_pct_label_ = nullptr;

    // ===== 阅读器 UI 组件（无顶栏，通栏正文卡）=====
    lv_obj_t *reader_page_ = nullptr;
    lv_obj_t *reader_top_label_ = nullptr;     // 卡内首行：《书名》 第N/M章 进度%
    lv_obj_t *reader_content_card_ = nullptr;
    lv_obj_t *reader_content_label_ = nullptr;

    // ===== 系统信息页 UI 组件 =====
    lv_obj_t *sys_time_label_ = nullptr;
    lv_obj_t *sys_sensor_label_ = nullptr;
    lv_obj_t *sys_wifi_icon_img_ = nullptr;
    lv_obj_t *sys_battery_icon_img_ = nullptr;
    lv_obj_t *sys_battery_pct_label_ = nullptr;
    lv_obj_t *sys_value_labels_[6] = {};       // CPU/运行/SRAM/PSRAM/电池/WiFi 的值

    // 图片图标（天气页状态栏）
    lv_obj_t *wifi_icon_img_ = nullptr;
    lv_obj_t *battery_icon_img_ = nullptr;
    lv_obj_t *battery_pct_label_ = nullptr;

    // 数据更新任务句柄
    TaskHandle_t update_task_handle_ = nullptr;

    // 省电模式：5 分钟无活动后降低刷新频率（1秒 → 5秒）
    std::atomic<bool> power_saving_{false};
    uint32_t last_activity_ms_ = 0;
    static const uint32_t IDLE_TIMEOUT_MS = 5 * 60 * 1000;
    static const int NORMAL_REFRESH_MS = 1000;
    static const int SAVING_REFRESH_MS = 5000;

    // 上次更新的值（用于避免不必要的 UI 刷新）
    int last_min_ = -1;
    time_t last_valid_epoch_ = 0;
    float last_temp_ = -99.0f;
    float last_humi_ = -99.0f;

    // LVGL flush 回调（将 RGB565 转换为 1-bit 并刷新到 RLCD）
    static void Lvgl_flush_cb(lv_display_t * disp, const lv_area_t * area, uint8_t * color_p);

    // UI 创建（实现在各 *_ui.cc）
    void SetupWeatherUI();
    void SetupMusicUI();
    void SetupPomodoroUI();
    void SetupReaderUI();
    void SetupSystemUI();
    void ApplyDisplayMode();

    // 顶栏中央「小智」+ 状态图标
    void CreateTopStatus(lv_obj_t *page);
    static void StatusIconDrawEvent(lv_event_t *e);
    void SetChatUiState(int st);

    // 天气页内部（调用者需已持有 DisplayLock）
    void RenderMemoWindow();     // 备忘录 2 行窗口渲染
    void ScanPhotos();           // 扫描 /sdcard/photos/*.png
    void ShowNextPhoto();        // 切换到下一张照片并按比例布局
    void RefreshBookList();      // 扫描书目
    void RenderBookWindow();     // 书目 4 行窗口渲染
    void LayoutBottomRow();      // 按照片宽度摆放照片卡 + 书目卡

    // 轮播定时器回调
    static void StatusAnimTimerCb(lv_timer_t *t);
    static void MemoTimerCb(lv_timer_t *t);
    static void PhotoTimerCb(lv_timer_t *t);
    static void BookTimerCb(lv_timer_t *t);
    static void VolumeHideTimerCb(lv_timer_t *t);

    // 阅读器内部（调用者需已持有 DisplayLock）
    void ReaderEnsureLoaded();
    void ReaderLoadChapter(int idx);
    void ReaderRenderPage();

    // 备忘录
    void LoadMemoFromNvs();

    // 数据更新任务（实现在 data_update_task.cc）
    static void DataUpdateTask(void *arg);

public:
    CustomLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                  int width, int height, int offset_x, int offset_y,
                  bool mirror_x, bool mirror_y, bool swap_xy, spi_display_config_t spiconfig, spi_host_device_t spi_host = SPI3_HOST);
    ~CustomLcdDisplay();

    // 获取 RLCD 驱动（供外部调用硬件方法，如对比度调节）
    RlcdDriver* rlcd() const { return rlcd_; }

    // 省电模式：记录用户活动，唤醒省电模式
    void NotifyUserActivity();
    bool IsPowerSaving() const { return power_saving_; }

    // AI 显示方法重写：AI 对话卡已移除，状态只在顶栏中央图标显示（SetChatUiState）
    // 保留空实现以兼容基类/应用层调用
    virtual void SetChatMessage(const char* role, const char* content) override;
    virtual void SetEmotion(const char* emotion) override;
    virtual void ClearChatMessages() override;

    // 重写状态栏更新（我们用图片图标，不用 Font Awesome 文字）
    virtual void UpdateStatusBar(bool update_all = false) override;

    // 重写主题切换（RLCD 单色屏不需要主题切换，避免基类操作不存在的控件导致崩溃）
    virtual void SetTheme(Theme* theme) override;
    virtual void SetMusicInfo(const char* title, const char* artist) override;
    virtual void SetMusicLyric(const char* lyric) override;
    virtual void SetMusicProgress(uint32_t current_ms, uint32_t total_ms) override;
    virtual void SwitchToMusicPage() override;
    virtual void SwitchToWeatherPage() override;

    // ===== 阅读器 =====
    void SwitchToReaderPage();
    bool IsReaderMode() const { return display_mode_ == MODE_READER; }
    void ReaderNextPage();
    void ReaderPrevPage();
    void ReaderNextChapter();
    void ReaderPrevChapter();
    bool ReaderOpenBook(const std::string& filename);
    std::vector<std::string> ReaderListBooks();

    // ===== 系统信息页 =====
    void SwitchToSystemInfoPage();
    bool IsSystemInfoMode() const { return display_mode_ == MODE_SYSTEM_INFO; }
    void UpdateSystemInfo();   // 读取 CPU/内存/电池/WiFi 并刷新卡内数值（自动加锁）

    // 启动数据更新任务（需要在网络连接后调用）
    void StartDataUpdateTask();

    // 刷新备忘录显示
    void RefreshMemoDisplay();
    void RefreshMemoDisplayInternal();
    void CycleDisplayMode();
    bool IsMusicMode() const { return display_mode_ == MODE_MUSIC; }
    bool IsPomodoroMode() const { return display_mode_ == MODE_POMODORO; }
    void SwitchToPomodoroPage();

    // 音量：调节后在音乐页显示临时胶囊 2 秒
    void ShowMusicVolume(int percent);

    // 番茄钟 UI 更新方法
    void UpdatePomodoroDisplay(const char* state_text, const char* countdown_text,
                               int progress_permille, const char* info_text);
};

#endif
