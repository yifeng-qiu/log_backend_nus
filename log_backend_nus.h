/**
 *@file log_backend_bt_nus.h
 * @author Yifeng Qiu (yifeng.q@gmail.com)
 * @brief
 * @version 0.1
 * @date 2023-05-31
 *
 * @copyright Copyright (c) 2023
 *
 */
#ifndef LOG_BACKEND_BT_NUS_H__
#define LOG_BACKEND_BT_NUS_H__

#ifdef __cplusplus
extern "C"
{
#endif

#define BLE_BUF_SIZE (CONFIG_BT_L2CAP_TX_MTU - 4)
    void register_bt_nus_auth_cbs(void);
    int nus_init(void);
    extern struct k_sem sem_nus_init_ok;

#ifdef __cplusplus
}
#endif

#endif /* LOG_BACKEND_BT_NUS_H__ */
