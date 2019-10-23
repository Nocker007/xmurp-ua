### 技术细节

#### 抓取数据包

模块根据握手时的第一个包（SYN，以后简称首包）来确定是否追踪这个 TCP 流以及修改的策略（保留包含 `"Windows NT"` 的 UA，还是替换掉所有的 UA）。当首包 mark 的 `0x10` 位被置为 1 时，追踪这条流。当首包 mark 的 `0x20` 位被置为 1 时，将保留包含 `"Windows NT"` 的 UA，否则将替换所有的 UA。

在确定追踪这条流的前提下，模块根据数据包的 mark 来确定是否抓取这个数据包。仅当 `0x10` 位被置为 1 时，模块会抓取这个数据包，否则不会抓取之。除了首包以外，`0x20` 位不起作用。如果一个数据包 mark 的 `0x10` 位被置为 1，但是所属的流没有被跟踪，或者这个包是服务端发来的（并且不是三次握手时的 SYN ACK 包），则会发出警告并丢弃（返回 `NF_DROP`）这个数据包。应该避免这样的情况出现。

总之，模块通过 `0x10` 确定一个数据包是否被捕获，通过首包（SYN）的 `0x20` 确定修改的策略，其它数据包的 `0x20` 被忽略。如果要追踪一条流，应该保证所有客户端到服务端的数据包的 `0x10` 位被置为 1、首包的 `0x20` 被置为合适的值、其它 IP 协议数据包的 `0x10` 被置为 0。模块中不会再作除上述内容以外的过滤，甚至不会判断目标端口是否为 `80`。在写防火墙规则时需要特殊考虑本地地址作为服务端的情况，以及（通常情况下）仅仅标记目标 `80` 端口的数据。

#### 流追踪的过程

我们首先假定不会发生乱序（TCP disorder）和丢包的情况，并假定追踪的流是合法的 HTTP 1.x 请求。捕获到首包之后，会开始追踪这条流并将状态置为 `rkpstm_established_sniffing`，然后返回 `NF_ACCEPT`。

主循环：当流的状态被置为 `rkpstm_established_sniffing` 时，每收到一个数据包（应用层长度不为零），都会截留（返回 `NF_STOLEN`，包括包含 HTTP 头的最后一个数据包）直到捕获到整个 HTTP 头部（在应用层中读到 `\r\n\r\n`）。当确认捕获到整个 HTTP 头部后，会根据情况检查并修改 UA，然后将截获的数据包发出，将状态置为 `rkpstm_established_waiting`（除非最后一个包就带有 PUSH）后返回 `NF_STOLEN`。之后不含 PUSH 的数据包都将直接返回 `NF_ACCEPT`。当捕获到包含 PUSH 的数据包时，会将流的状态置为 `rkpstm_established_sniffing`，返回 `NF_ACCEPT`。对于不含应用层数据的包（单纯的 ACK 包），会直接放行并不修改状态。

为了处理乱序和丢包的情况，模块会记录建立连接时的序列号，并在每次返回 `NF_ACCEPT` 时更新这个序列号。对于每个收到的数据包，如果序列号不符合期待，则进行判断：若在期待的序列号之前 `0x80000000` 的范围内，视作重传，直接返回 `NF_ACCEPT`；否则，说明发生了乱序，将这个包放到缓存区延迟处理，返回 `NF_STOLEN`。每处理完成一个数据包后，确认缓存区是否有序列号符合期待的数据包并处理。因为实际情况下路由器上转发到外网的包经过的网络环境很简单，乱序出现的概率非常小（测试至少小于千分之一），所以不会造成性能的问题。

捕获过程中，如果发现 HTTP 头的长度超过 64 个数据包，或者在收集到完整的头部之前就收到 PSH，则认为不是有效的 HTTP 1.x 请求，会发出警告，将截获的数据包发出，返回 `NF_ACCEPT`。

为了应对没有发出 FIN 和 ACK 就挂掉的连接（尤其是现在 keep-alive 非常流行），模块干脆不会特殊处理 FIN 和 RST，而是：在建立每条连接、以及每条连接的包经过的时候，同时记录时间（精确到秒就足够了）。每隔 1200 秒后，收到一个数据包的时候会触发清理的动作，超过 1200 秒没有活动的流就会被清理。因为使用源端口号来查找对应的流，因此不会造成查找太慢，只是确实稍稍浪费内存（应该可以忽略不计）。

另外，当一个新的连接的两个地址和两个端口与一个旧的连接都相同的时候，模块会将旧的连接覆盖掉。

#### 假装面向对象

仿照 MBROLA 的设计，为了让代码容易整理，用面向对象的思路。每一个头文件即是一个类（本质上是一个结构体，和很多个以这个结构体的指针为第一个参数的函数）。`xxx` 类的函数都以 `xxx_` 开头。`xxx_new`、`xxx_del` 分别是 `xxx` 类的构造函数和析构函数。除了 `sk_buff` 中的数据，变量都以本机的字节序存储。

##### `rkpStream` 类

存储一个流的信息。

成员变量：

* `u_int8_t enum {...} status`：流的状态。
* `u_int32_t id[3]`：这 16 个字节按顺序存储源地址、目标地址、源端口、目标端口。
* `struct sk_buff* buff`：截取的数据包。保证已经 `skb_ensure_writable`。
* `struct sk_buff* buff_prev`：由于乱序而需要暂缓处理的数据包。保证已经 `skb_ensure_writable`。
* `u_int32_t seq`：存储下一个期待收到的包的序列号。
* `time_t last_active`：记录最后一次活动的时间（单位为秒）。超过 1200 秒没有活动的连接会被放弃。
* `u_int8_t scan_matched`：在 `rpstm_established_sniffing` 状态下，指示直到 `buff` 中最后一个包的应用层末尾，匹配 `"\r\n\r\n"` 的字节数。一旦匹配到，则会短暂地被用来记录匹配 `"User-Agent: "` 等其它字符串的进度。当它没有意义时，总是被置为 0。
* `u_int8_t windows_preserve`：当 UA 中包含 `"Windows NT"` 时，是否保留不变。实际上，如果不需要保留，根本就不需要去确认是否包含 `"Windows NT"`。
* `struct rpStream* next`：单向链表用。

成员函数：

* `struct rkpStream* rpStream_new(struct sk_buff*)`：构造函数。传入一个数据包，用它上面的数据来构造流；传入的不是 SYN 也可以。

* `void rkpStream_del(struct rkpStream*)`：析构函数。

* `u_int32_t rkpStream_judge(struct rkpStream*, struct sk_buff*)`：检查一个数据包。保证只是检查，不会变动数据包或流的任何内容。

  返回值的低八位用于通知上层函数如何处理，不同位表示不同的意义：

  * `0x01` 位：表示这个包是否属于这个流。置为 1 时表示属于。
  * `0x02` 位：表示这个包是否可能需要修改。置为 1 时表示可能需要。
  * `0x04` 位：表示这个包是否需要被截留。置为 1 时表示需要。
  * `0x08` 位：发生了某些错误（比如，没有找全 HTTP 头却要 PSH，或者读取已经截取的应用层数据的前几个字节知道不是支持的 HTTP 方法，等）。上层函数应该向用户发出警告，将所有流中的数据放出，并停止模块的运行。

  事实上，可能的返回结果和处理方法：

  * `rtn & 0x01 == 0x00`：不属于这个流。当然是去尝试下一个或者宣布这个包被错误地抓到啦。
  * `else`：稍后需要调用 `rkpStream_execute`，但在这之前和之后需要还需要根据不同情况做一点事情。
    * `rtn & 0x02 == 0x02`：确保这个包可以修改（调用 `skb_ensure_aritable`）。否则不用调用。
    * `rtn & 0x04 == 0x04`：返回 `NF_STOLEN`。否则返回 `NF_ACCEPT`。当然，实际上，被截留的包一定需要确保可修改，但这不需要在代码中体现。

  高 24 位用于通知 `rkpStream_execute` 如何处理。其中，低四位共同表示包的处理方法，以及其它位的意义：

  * `0x0`：包不属于这个流。除了这种情况以外，都需要更新时间，以后不再重复。
  * `0x1`：包属于这个流，但直接放行就可以（不携带应用层数据，或者是重传）。
  * `0x2`：包是属于未来的、包含有应用层数据的数据包，需要置入 `buff_prev`。除了这三个以外，都需要更新序列号，不再重复。
  * `0x3`：包是 HTTP 头部的一部分，并且即使算上这个数据包，也没有收集到完整的头部。仅仅在这时，之后的四位才有用，它用来标记 `"\r\n\r\n"` 已经匹配了几个字符。
  * `0x4`：包是 HTTP 头部的一部分，算上这个数据包，已经收集到了完整的头部，并且头部是在这个数据包中结束的。只有在这时，高 16 位才有用，它用来标记 `"\r\n\r\nx"` 中，`'x'` 的位置相对于这个包中应用层第一个字节的偏移。
  * `0x5`：包不是 HTTP 头部的一部分，放行即可。是否设置 `rkpstm_established_sniffing` 通过直接读取数据包的内容确定。它与 `0x1` 的区别在于，是否需要更新序列号。

* `void rkpStream_execute(struct rkpStream*, struct sk_buff*, u_int32_t)`：执行需要执行的动作。第三个参数传入 `rkpStream_judge` 对于这个包的返回值，它的低八位会被忽略。它要做的事情几乎完全由 `rkpStream_judge` 的返回值来通知。以下数值是指 `rtn & 0x0f00 >> 8` 的值。

  * `0x0`：返回。
  * `0x1`：更新时间，然后返回。
  * `0x2`：确认序列号是否符合期待。
    * 符合期待则丢入 `buff`，更新时间、序列号、扫描状况。然后，循环确认 `buff_prev` 中是否有符合序列号的，直到没有为止。
      * 有的话拿出来，调用 `rkpStream_judge`，看返回值是 `0x2` 还是 `0x3`。只可能是这两个之一。
        * 如果是 `0x2`，丢入 `buff`，更新时间、序列号、扫描状况。
        * 如果是 `0x3`，丢入 `buff`，更新时间、序列号、扫描状况。调用 `__rkpStream_modify` 来修改 UA，调用 `__rkpStream_flush` 来将截留的所有包发出（最后一个参数为 0）。
      * 

* `int8_t __rpStream_check_sequence(struct rpStream*, struct sk_buff*)`：视为私有变量。检查一个数据包是重传（返回 -1）、乱序（返回 1）还是正常的（返回 0）。

* 

#### 其它细节

* 为什么通过第二个包而不是第三个包来判定连接已经建立？

  因为我查了查，原则上第三个包是可以携带应用层数据的。考虑到这一点，如果用第三个判定的话，就会让步骤变得比较混乱。

* TCP 不是不区分服务端和客户端吗？

  但是 HTTP 区分啊。

* 为啥文档要写这么详细？

  之前在公司实习的时候，要求我写这样详细。后来我觉得这样挺好的。实际上我是先写文档后写代码的。