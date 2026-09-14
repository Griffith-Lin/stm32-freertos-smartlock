# I2C 总线层参数化:一份时序代码服务两条总线

> 一句话定位:**时序是协议的一部分(不变),引脚是硬件的一部分(每条总线不同)—— 把引脚抽成参数,时序代码就只需要一份。**
> 关键代价:**bit-band 用不了了。** 它的地址是编译期常量,而参数是运行时才知道的。出路是换成 `BSRR`/`IDR`,把"哪个引脚"从**地址**里搬进**数据**里。

---

## 1. 起因:两条总线,一份代码

板上有两个 I2C 器件,挂在**不同**的引脚上:

| 器件 | SCL | SDA | 备注 |
| --- | --- | --- | --- |
| CY8CMBR3116 电容按键 | PB6 | PB7 | 已经在跑 |
| AT24C04A EEPROM | PA8 | PC9 | 新增,两条线还跨了端口 |

这是同一件事:**协议相同,引脚不同**。可选的三条路:

| 方案 | 做法 | 问题 |
| --- | --- | --- |
| ① 复制一份 | 把 `i2c.c` 复制成 `i2c2.c` 再改名 | 两份一模一样的中文注释,以后改一个 bug 要改两处,必漏 |
| ② 全局"当前总线" | `i2c_bus_select(&bus)` 之后函数不带参数 | 引入**隐藏状态**:两个任务互相踩,而且看调用点不知道在操作哪条总线 |
| ③ **参数化** ✅ | 每次调用显式传总线 | 调用点啰嗦一点,但状态全在明面上 |

选了 ③。

## 2. 设计:把"总线"做成一个结构体

```c
typedef struct
{
    GPIO_TypeDef *scl_port;  // SCL 所在端口
    uint16_t      scl_pin;   // SCL 引脚号(GPIO_Pin_x)
    GPIO_TypeDef *sda_port;  // SDA 所在端口
    uint16_t      sda_pin;   // SDA 引脚号
    uint32_t      rcc_ahb1;  // 这两个端口所属的 AHB1 时钟使能位
} i2c_bus_t;

extern const i2c_bus_t i2c_bus_cy9cm;    // PB6 / PB7
extern const i2c_bus_t i2c_bus_at24c04;  // PA8 / PC9
```

所有时序函数首参加上 `const i2c_bus_t *bus`,函数名从 `i2c_master_Config()` 改为 `i2c_bus_config()`。

### 2.1 三个设计细节

**① 结构体是 `const`,定义在 `i2c.c` 里,`i2c.h` 里只留 `extern` 声明。**

不只是"以防被改"。`const` 让链接器把它放进**只读段**,直接躺在 flash 里,一个字节 RAM 都不占。链接 map 文件里的证据:

```
i2c_bus_cy9cm        0x08004564   Data   20   i2c.o(.constdata)
i2c_bus_at24c04      0x08004578   Data   20   i2c.o(.constdata)
```

地址在 `0x0800_xxxx`,那是 flash 区(ROM 从 `0x08000000` 起)。想改都改不进去。

> `Data` 20 字节 = 结构体实际大小。两个指针 8 + 两个 `uint16_t` 4 + 一个 `uint32_t` 4 = 16,再加两处对齐填充(见下面),共 20。
>
> ```
> 偏移 0  scl_port   (4)
> 偏移 4  scl_pin    (2)
> 偏移 6  ── 填充 2 字节 ──   ← 为了下面这个 4 字节指针对齐到 4 的倍数
> 偏移 8  sda_port   (4)
> 偏移 12 sda_pin    (2)
> 偏移 14 ── 填充 2 字节 ──
> 偏移 16 rcc_ahb1   (4)
> ```

**② 传指针,不传结构体本身。** 20 字节的结构体按值传,每次调用都要在栈上拷一份;传 `&i2c_bus_cy9cm` 就是把那个 flash 地址塞进寄存器,几步指令搞定。

**③ 定义放 `.c` 不放 `.h`。** 如果常量定义写在头文件里,每个 `#include` 它的 `.c` 都会生成一份同名常量 → 链接期 repeated symbol 冲突。头文件只准放 `extern` 声明。

## 3. 第一个坎:bit-band 没法参数化

### 3.1 原来是这么写的

```c
#define I2C_SCL PBout(6)
#define I2C_SDA PBout(7)
```

展开 `PBout(6)`,顺着 [api/bitband.h](../api/bitband.h) 的宏算一遍:

```c
#define BITBAND(addr,bit) (((addr&0xF0000000)+0x2000000)+ ((addr&0xFFFFF)*8+bit)*4)
#define PBout(bit)  MEMADDR((unsigned int)&GPIOB->ODR, bit)
#define MEMADDR(addr,bit)  *(unsigned int *)BITBAND(addr,bit)
```

- `&GPIOB->ODR` = `GPIOB_BASE + 0x14` = `0x40020000 + 0x400 + 0x14` = `0x40020414`
- `addr & 0xF0000000` = `0x40000000` → `+ 0x2000000` = 别名区基址 `0x42000000`
- `(addr & 0xFFFFF)` = `0x20414` → `× 8 + 6` = `0x1020A6` → `× 4` = `0x408298`

```
PBout(6)  →  *(unsigned int *)0x42408298
PBout(7)  →  *(unsigned int *)0x4240829C
```

写 1 就是置位,写 0 就是清零,一行搞定,很方便。

### 3.2 问题出在哪

**这个地址是编译期算死的。** 编译器把它当成一个常量地址直接编进指令:

```asm
MOV  r1, #0x4240829C      ; 地址写死在指令里
STR  r1, [r0]             ; 存值
```

引脚号 `6`/`7` 必须是个常量,因为它是**算地址用的**。而参数化之后,引脚号是运行时从 `bus->scl_pin` 里读出来的变量 ——

> **`GPIOB` 这条路径和 `GPIOC` 这条路径的别名地址差很远,想让同一条指令两个都覆盖到,除非把地址也算成变量。**
> 可地址一旦是变量,bit-band 就没剩下任何好处了 —— 它存在的意义就是"用编译期算术换掉运行时的读-改-写"。

所以必须换一种表达方式:**把"哪个引脚"从地址里搬出来,放进要写的数据里**。

---

## 4. 出路:BSRR 寄存器

### 4.1 位布局:一个 32 位寄存器,劈成两个 16 位半区

```
bit 31 ........................ bit 16 | bit 15 ........................ bit 0
└──────────── BR (Reset 复位的 16 位) ──┴──────────── BS (Set 置位的 16 位) ──┘
```

规则只有一条:**写 1 才动作,写 0 什么都不干。**

- 往**低 16 位**的 bit n 写 1 → **引脚 n 置 1**
- 往**高 16 位**的 bit n 写 1 → **引脚 n 清 0**

同一半区里没写 1 的那些位就是"不管",所以**一次写可以把某些脚置 1、另一些脚清 0** —— 这正是它劈成两半的设计目的。

### 4.2 封装成函数

```c
static void i2c_scl_write(const i2c_bus_t *bus, uint8_t v)
{
    bus->scl_port->BSRR = v ? bus->scl_pin : ((uint32_t)bus->scl_pin << 16);
}

static void i2c_sda_write(const i2c_bus_t *bus, uint8_t v)
{
    bus->sda_port->BSRR = v ? bus->sda_pin : ((uint32_t)bus->sda_pin << 16);
}

static uint8_t i2c_sda_read(const i2c_bus_t *bus)
{
    return (bus->sda_port->IDR & bus->sda_pin) ? 1 : 0;
}
```

### 4.3 那个三元运算符在干什么

⚠️ **`v` 不是"要写进寄存器的值",它只负责选。** 写进去的是三元表达式的结果,里面根本没有 `1`。

展开成 if/else 就一目了然:

```c
if (v)   // v 只是条件
{
    bus->sda_port->BSRR = bus->sda_pin;                    // 置位半区
}
else
{
    bus->sda_port->BSRR = ((uint32_t)bus->sda_pin) << 16;  // 复位半区
}
```

代入 `i2c_sda_write(&i2c_bus_at24c04, 1)`(`sda_pin = GPIO_Pin_9 = 0x0200`):

| 步骤 | 结果 |
| --- | --- |
| ① 算条件 `v ?` | `v = 1`,非 0 → 真 |
| ② 取真分支 `bus->sda_pin` | `0x00000200` |
| ③ 存入 BSRR | 写入 `0x00000200` → bit9 = 1 → **BS9 = 1 → PC9 置 1** |

换成 `v = 0`:

| 步骤 | 结果 |
| --- | --- |
| ① 算条件 `v ?` | `v = 0` → 假 |
| ② 取假分支 `sda_pin << 16` | `0x02000000` |
| ③ 存入 BSRR | 写入 `0x02000000` → bit25 = 1 → **BR9 = 1 → PC9 清 0** |

**`<< 16` 干的事就是"把引脚号从置位半区搬到复位半区"**:引脚 9 → 复位位 9+16 = 25。

### 4.4 完整对照表

| 调用 | 端口 | 写入 BSRR 的值 | 命中的位 | 结果 |
| --- | --- | --- | --- | --- |
| `i2c_scl_write(&i2c_bus_cy9cm, 1)` | GPIOB | `0x00000040` | BS6 | PB6 松手(靠上拉拉高) |
| `i2c_scl_write(&i2c_bus_cy9cm, 0)` | GPIOB | `0x00400000` | BR6 | PB6 拉低 |
| `i2c_sda_write(&i2c_bus_cy9cm, 1)` | GPIOB | `0x00000080` | BS7 | PB7 松手 |
| `i2c_sda_write(&i2c_bus_at24c04, 1)` | GPIOA | `0x00000100` | BS8 | PA8 松手 |
| `i2c_sda_write(&i2c_bus_at24c04, 0)` | GPIOC | `0x02000000` | BR9 | PC9 拉低 |

（置位半区 `0x0200 << 16` 得 `0x02000000`,bit25 = 9 + 16 ✓）

### 4.5 读回来:IDR

```c
static uint8_t i2c_sda_read(const i2c_bus_t *bus)
{
    return (bus->sda_port->IDR & bus->sda_pin) ? 1 : 0;
}
```

`IDR & 引脚掩码` 取出那一位。**开漏输出时 IDR 读到的是总线上的真实电平,不是 ODR 里写的值** —— 所以从机拉低 SDA 时能读到 0,靠这个才能判断 ACK/NACK。

原来的 `GPIO_ReadInputDataBit(GPIOB, GPIO_Pin_7)` 内部也是读 IDR,只是把端口和引脚写死了。

---

## 5. 两个容易写错的地方

### 5.1 `(uint32_t)` 强转不能省

```c
bus->scl_port->BSRR = v ? bus->scl_pin : ((uint32_t)bus->scl_pin << 16);
                                            └────┬────┘
                                            少了这个会出事
```

`bus->scl_pin` 是 `uint16_t`,在 C 里做运算前会**整数提升**成 `int`,于是 `pin << 16` 是在**有符号 int** 里算的:

| 引脚号 | `GPIO_Pin_x` 值 | `<< 16` 之后 | 有没有问题 |
| --- | --- | --- | --- |
| 0 ~ 14 | 0x0001 ~ 0x4000 | 最高到 0x40000000 | 没超 `int` 上限,侥幸正确 |
| **15** | **0x8000** | **0x80000000** | **超出 int 正数范围 → 未定义行为(UB)** |

我们用的引脚(6/7/8/9)都不会触发,但这是必须堵上的口子。提前转成 `uint32_t`,左移就变成**无符号运算**,溢出行为由 C 标准明确规定为回绕,结果稳定是 `0x80000000`。

### 5.2 必须是 `=` 赋值,不能写 `|=`

```c
bus->sda_port->BSRR = bus->sda_pin;      // ✅
bus->sda_port->BSRR |= bus->sda_pin;     // ❌ 别这么写
```

`BSRR` 本来就是**只写寄存器**(手册里 BR 那半区明确标着 write-only、读回 0),`|=` 会先读一次那个读不出内容的寄存器。虽然在本芯片上恰好也能出正确结果,但那是巧合,不是设计意图。

---

## 6. 为什么不用 `ODR |= ...`

`GPIOx->ODR` 也能写引脚,但它是**读-改-写**三步:

```
① 读 ODR  →  ② 或上这一位  →  ③ 写回 ODR
```

如果读和写之间来了中断、而中断里也动了**同一个端口**的别的引脚 —— 中断改完的 ODR,会被第 ③ 步写回的结果**整个覆盖掉**,那次修改就丢了。

`BSRR` 是**一条 32 位存储指令**,硬件自己完成置位/复位,没有中间态,不可能被中断打断出岔子。这就是它存在的理由。

顺带一提,StdPeriph 自己的 `GPIO_SetBits()` / `GPIO_ResetBits()` 也是这么实现的([lib/src/stm32f4xx_gpio.c:411](../lib/src/stm32f4xx_gpio.c#L411)):

```c
void GPIO_SetBits(GPIO_TypeDef* GPIOx, uint16_t GPIO_Pin)
{
  GPIOx->BSRR = GPIO_Pin;                    // 置位
}
void GPIO_ResetBits(GPIO_TypeDef* GPIOx, uint16_t GPIO_Pin)
{
  GPIOx->BSRR = (uint32_t)GPIO_Pin << 16;    // 复位
}
```

和我们手写的两个函数是同一个套路,可以放心。

---

## 7. 一句代码,两条总线路由到不同地址

`bus->sda_port->BSRR` 里的 `->` 是 `(*...).` 的简写,分两步解引用:

1. `bus->sda_port` —— 取出端口指针,类型 `GPIO_TypeDef *`
2. `(*(...)).BSRR` —— 顺指针找到寄存器组,取 `BSRR` 成员

**结构体成员的偏移量在编译期就是固定的**,`GPIO_TypeDef` 里寄存器按地址顺序排列:

| 成员 | 偏移 | GPIOC 的实际地址 |
| --- | --- | --- |
| MODER | 0x00 | 0x40020800 |
| OTYPER | 0x04 | 0x40020804 |
| OSPEEDR | 0x08 | 0x40020808 |
| PUPDR | 0x0C | 0x4002080C |
| IDR | 0x10 | 0x40020810 |
| ODR | 0x14 | 0x40020814 |
| **BSRR** | **0x18** | **0x40020818** |

（`GPIOC_BASE` = `AHB1PERIPH_BASE + 0x0800` = `0x40020000 + 0x0800` = `0x40020800`）

所以 `bus->sda_port->BSRR` 编译出来就是 **`bus->sda_port` 的值 + 0x18**。同一句代码,换个 `bus` 参数就落到完全不同的地址:

| 调用 | `sda_port` | 实际 BSRR 地址 | 写入值 | 结果 |
| --- | --- | --- | --- | --- |
| `i2c_sda_write(&i2c_bus_at24c04, 1)` | GPIOC | **0x40020818** | 0x00000200 | PC9 松手 |
| `i2c_sda_write(&i2c_bus_cy9cm, 1)` | GPIOB | **0x40020418** | 0x00000080 | PB7 松手 |

**对比 bit-band:** `PBout(7) = 1` 展开成 `*(volatile uint32_t *)0x4240829C = 1`,地址写死在指令里,根本表达不了 `0x40020818` 那种情况。

> 一句话总结这次替换的本质:
> **bit-band 把信息藏在地址里 —— 地址不能参数化。**
> **BSRR 把信息放在数据里 —— 数据可以参数化。**

---

## 8. 为什么两条总线不会打架

「共用一份代码」和「共用一份状态」是两件事。`i2c.c` 被两边共用的**只有代码**。

### 8.1 没有任何可变的共享量

| 东西 | 类型 | 在哪儿 |
| --- | --- | --- |
| `i2c_bus_cy9cm` / `i2c_bus_at24c04` | **`const`** | 只读,存在 **flash** 里 |
| 三个 `static` 辅助函数 | 函数 | 没有 `static` 局部变量 |
| `ack`、`data`、`i` | 自动变量 | 各自函数的**栈**上 |

关键在最后一行:自动变量每次调用都在**调用者自己的栈帧**上重新开一块。两个任务各调一次 `i2c_master_write()`,各自的 `ack` 是两个不同的内存地址 —— **同名不同物**。

### 8.2 物理上本来就是两条独立总线

不同引脚、不同走线、各自独立的上拉电阻 —— 板上就是两个互不相连的网,想干扰都没有电气通路。

### 8.3 寄存器层面唯一的交叉点也不冲突

PA8 在 **GPIOA** 上,CY9CM 的 RES 脚 PA1 也在 GPIOA 上 —— 这是唯一重叠的地方。

但 BSRR **按位操作,写 0 的位不动作**:

```c
GPIOA->BSRR = 0x00000100;   // bit8  = 1 → 只动 PA8
GPIOA->BSRR = 0x00020000;   // bit17 = 1 → 只动 PA1(高半区位 1+16)
```

两次写互相完全看不见对方。而且各自都在**一条指令里原子完成**,不存在"读到旧值 → 改 → 写回 → 覆盖别人"的窗口。

### 8.4 真正会打架的情况

**同一个 `bus` 被两个任务同时使用。**

```
任务 A: SCL=0 → SDA=1 → SCL=1 → ...   ← 正发到第 3 位
        ↓ 任务切换
任务 B: SCL=0 → SDA=0 → SCL=1 → ...   ← 从 START 重新开始,把 A 的波形搅烂
```

两边在**同一对引脚**上各按自己的节奏翻转,从机收到的是两个事务的碎片。

**解决办法:给这条总线配一个互斥量**,每次事务前 take、结束后 give。

```c
SemaphoreHandle_t i2c_lock_at24c04 = xSemaphoreCreateMutex();
// 事务前后
xSemaphoreTake(i2c_lock_at24c04, portMAX_DELAY);
...  AT24C04_read(...)  ...
xSemaphoreGive(i2c_lock_at24c04);
```

判断标准:

| 情况 | 要锁吗 |
| --- | --- |
| 一条总线只有一个任务碰 | 不用 |
| 一条总线两个以上任务读写 | **必须加锁** |
| 中断里也要用同一条总线 | 互斥量不够(不能在 ISR 里 take),得用临界区或专用任务 |

---

## 9. 改动清单

| 文件 | 改了什么 |
| --- | --- |
| [api/i2c.h](../api/i2c.h) | 新增 `i2c_bus_t` 结构体 + 两个 `extern const` 总线常量;所有函数加 `const i2c_bus_t *bus` 首参;`i2c_master_Config()` → `i2c_bus_config()` |
| [api/i2c.c](../api/i2c.c) | 定义两个总线常量;`PBout`/`PBin` 换成 `BSRR`/`IDR` 三个静态辅助函数;时序波形**一个字节没动**,中文注释原样保留 |
| [api/AT24C04.c](../api/AT24C04.c) | 18 处调用点全部补上 `&i2c_bus_at24c04` |
| [api/CY9CM.c](../api/CY9CM.c) | 15 处调用点全部补上 `&i2c_bus_cy9cm` |
| [user/main.c](../user/main.c) | `i2c_master_Config();` → `i2c_bus_config(&i2c_bus_cy9cm);` + `i2c_bus_config(&i2c_bus_at24c04);` |

验证方式:编译通过,三个文件 **0 warning 0 error**。

---

## 10. 坑清单

1. **bit-band 不能参数化**。碰到"运行时才知道是哪个引脚"的场景,就必须换 BSRR/IDR。这是本次改造的核心。
2. **BSRR 必须 `=` 赋值,不能 `|=`**。它只写不读。
3. **`(uint32_t)` 强转不能省**。`GPIO_Pin_15 = 0x8000`,`0x8000 << 16` 在有符号 int 里是 UB。
4. **别用宏把 bus 参数藏起来**。见过 `#define i2c_master_start() i2c_master_start(&cur_bus)` 这种写法 —— 省了几个字符,却把"操作哪条总线"这个关键信息从调用点上抹掉了,还把编译器的参数检查绕过去了。宁可多打几个字。
5. **改函数签名后老调用点会集体报错**,这是好事 —— 报错列表就是一份"要改的地方"清单,一个个改完再编译。
6. **跨端口的引脚时钟要开两个**。PA8 + PC9 分属 GPIOA 和 GPIOC,`rcc_ahb1` 字段用 `|` 合并:`RCC_AHB1Periph_GPIOA | RCC_AHB1Periph_GPIOC`。`RCC_AHB1PeriphClockCmd()` 内部是 `RCC->AHB1ENR |= ...`([lib/src/stm32f4xx_rcc.c:2105](../lib/src/stm32f4xx_rcc.c#L2105)),所以合并位是安全的。
7. **`const` 常量定义放 `.c`,头文件只放 `extern`**。放头文件里每个包含它的 `.c` 都会生成一份,链接期冲突。

---

## 11. 自测(答案在文末,先别看)

1. 为什么 `PBout(6) = 1` 这种写法没法参数化?问题的根子在哪一步?
2. `i2c_sda_write(&i2c_bus_at24c04, 1)` 里那个 `1`,最终有没有被写进 BSRR?如果没有,写进去的是什么?
3. BSRR 里 bit 9 和 bit 25 分别控制 PC9 的什么动作?
4. `((uint32_t)bus->sda_pin << 16)` 里的 `(uint32_t)` 去掉,程序会怎样?请具体说出哪几个引脚会出问题。
5. `bus->sda_port->BSRR` 为什么地址是"端口基址 + 0x18"?
6. 两条总线共用 `i2c.c`,为什么不需要加互斥量?什么情况下就必须加了?
7. 如果把 `i2c_bus_cy9cm` 的 `const` 去掉,会有什么变化?

---

<details>
<summary>答案(点开)</summary>

1. 因为 `PBout(6)` 展开后是一个**编译期算死的 bit-band 别名地址**(`0x42408298`),引脚号 `6` 是**参与地址计算**的。引脚号一旦变成运行时变量,地址也就成了变量 —— 而 bit-band 存在的全部意义就是"用编译期算术换掉运行时的读-改-写",地址成了变量它就没好处了。出路是把"哪个引脚"从地址搬到**要写的数据**里,也就是 BSRR。

2. **没有**。`1` 是三元的**条件** `v ?`,只负责选分支,自己不进寄存器。真正写进 BSRR 的是真分支的值 `bus->sda_pin`,对 `i2c_bus_at24c04` 来说是 `GPIO_Pin_9 = 0x0200`,所以 BSRR 收到的是 `0x00000200`。

3. **bit 9(置位半区)= PC9 置 1**,开漏下表现为"松手",靠外部上拉把线拉到 3.3V;**bit 25(复位半区)= PC9 清 0**,即 N-MOS 导通把线拉到 GND。25 = 9 + 16,`<< 16` 就是做这个搬移。

4. 会变成**未定义行为**,因为 `uint16_t` 会先整数提升成有符号 `int`,`pin << 16` 就在 int 里算。具体说:**`GPIO_Pin_15`(`0x8000`)会出问题** —— `0x8000 << 16 = 0x80000000` 超出 `int` 的正数范围。引脚 0~14 的值最大到 `0x4000`,左移后是 `0x40000000`,还没超,侥幸正确。虽然本工程用的 6/7/8/9 都不触发,但这类"某个引脚号刚好踩线"的 UB 是最难查的,所以统一加 `(uint32_t)` 防御。

5. 因为 `GPIO_TypeDef` 里寄存器是**按芯片地址顺序**排列的,每个占 4 字节:`MODER`(0x00) → `OTYPER`(0x04) → `OSPEEDR`(0x08) → `PUPDR`(0x0C) → `IDR`(0x10) → `ODR`(0x14) → **`BSRR`(0x18)**。编译器算 `->BSRR` 就是"基址 + 0x18",基址来自 `bus->sda_port` 这个**运行时**指针,所以同一句代码换个 `bus` 就落到不同地址。

6. 因为 `i2c.c` 里**没有任何可变的共享状态**:两个总线常量是 `const`(在 flash 里),三个辅助函数没有 `static` 局部变量,其余全是自动变量(各在自己的栈帧上)。共用的是代码,不是数据。**当同一个 `bus` 被两个及以上任务同时使用时**就必须加互斥量 —— 那时两边会在同一对引脚上各按各的节奏翻转,波形互相搅烂。(中断里也要用同一条总线的话,互斥量还不够,得用临界区。)

7. 最大的变化是**它会从 flash 被搬到 RAM**,20 字节占 RW 段,还要在启动时从 flash 拷贝初值过去。同时失去"改不进去"的保护 —— 万一有指针越界写到那儿,总线描述就被改写,表现为"某个引脚莫名其妙失灵"。**这 20 字节的 RAM 完全不值得省。**

</details>
