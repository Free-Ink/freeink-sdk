#include "epd_board.h"

#include <esp_log.h>
#include <stddef.h>

#include "epdiy.h"

/**
 * The board's display control pin state. 
 * 全局控制寄存器状态。
 * 用于保存当前 EPD 相关控制引脚的逻辑状态，
 * 如 ep_output_enable, ep_mode, ep_sth, ep_stv, ep_latch_enable 等。
 * 最终会通过 epd_board->set_ctrl() 写入硬件引脚。
 */
static epd_ctrl_state_t ctrl_state;

/**
 * The EPDIY board in use.
 */
const EpdBoardDefinition* epd_board = NULL;

/**
 * busy 等待（延时）。
 * 使用 CPU 周期计数器进行延时，不依赖 RTOS。
 * IRAM_ATTR 表示放入指令 RAM，加快执行速度。
 */
void IRAM_ATTR epd_busy_delay(uint32_t cycles) {
    volatile unsigned long counts = XTHAL_GET_CCOUNT() + cycles;
    while (XTHAL_GET_CCOUNT() < counts) {
    };
}

/**
 * 设置当前使用的 EPD 板（仅能设置一次）。
 * 通常在上电初始化时由用户调用：
 *
 *    epd_set_board(&board);
 */
void epd_set_board(const EpdBoardDefinition* board_definition) {
    if (epd_board == NULL) {
        epd_board = board_definition;
    } else {
        ESP_LOGW("epdiy", "EPD board can only be set once!");
    }
}

/**
 * 获取当前使用的 EPD 板定义。
 */
const EpdBoardDefinition* epd_current_board() {
    return epd_board;
}

/**
 * 设置 EPD “模式”位和“输出使能”位。
 * state = true 则两个信号都被设置为1。
 *
 * 最后调用 epd_board->set_ctrl() 将修改同步到硬件。
 */
void epd_set_mode(bool state) {
    ctrl_state.ep_output_enable = state;
    ctrl_state.ep_mode = state;

    // 掩码：表示本次写入哪些字段需要生效
    epd_ctrl_state_t mask = {
        .ep_output_enable = true,
        .ep_mode = true,
    };
    
    // 将修改写入实际硬件
    epd_board->set_ctrl(&ctrl_state, &mask);
}

/**
 * 返回控制寄存器的当前状态指针，
 * 供其他模块读取或修改。
 */
epd_ctrl_state_t* epd_ctrl_state() {
    return &ctrl_state;
}

/**
 * 初始化 EPD 控制寄存器，在 EPD 初始化流程中调用。
 * 设置所有控制信号为默认值。
 */
void epd_control_reg_init() {
    ctrl_state.ep_latch_enable = false;
    ctrl_state.ep_output_enable = false;
    ctrl_state.ep_sth = true;
    ctrl_state.ep_mode = false;
    ctrl_state.ep_stv = true;

    // mask 指定哪些位需要写入
    epd_ctrl_state_t mask = {
        .ep_latch_enable = true,
        .ep_output_enable = true,
        .ep_sth = true,
        .ep_mode = true,
        .ep_stv = true,
    };

    // 应用到硬件
    epd_board->set_ctrl(&ctrl_state, &mask);
}

/**
 * 在 deinit (关机) 时调用。
 * 关闭输出，使能低功耗。
 */
void epd_control_reg_deinit() {
    ctrl_state.ep_output_enable = false;
    ctrl_state.ep_mode = false;
    ctrl_state.ep_stv = false;
    epd_ctrl_state_t mask = {
        .ep_output_enable = true,
        .ep_mode = true,
        .ep_stv = true,
    };
    epd_board->set_ctrl(&ctrl_state, &mask);
}
