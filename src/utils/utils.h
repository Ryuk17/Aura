/* Aura — WeNet 工具头的兼容层
 *
 * 只收"引进来的 WeNet 源码用得到、而 Aura 这边没有对应物"的那几个宏。
 * 刻意不照搬 WeNet 的 utils.h：那边大部分内容（string 切分、时间戳、文件读）
 * Aura 要么在 utils/ 下有自己的实现，要么根本用不上，一起搬进来只会变成
 * 第二套没人维护的工具集。
 *
 * 目前只有 blocking_queue.h 用到这一个宏。
 */
#ifndef AURA_UTILS_UTILS_H
#define AURA_UTILS_UTILS_H

/* C++11 起有 = delete，不需要再走私有声明那套老写法。 */
#define WENET_DISALLOW_COPY_AND_ASSIGN(type) \
    type(const type &) = delete;             \
    type &operator=(const type &) = delete;

#endif /* AURA_UTILS_UTILS_H */
