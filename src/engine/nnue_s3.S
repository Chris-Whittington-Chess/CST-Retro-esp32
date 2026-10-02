// Chess System Tal Retro - ESP32-S3 PIE SIMD kernels for the NNUE: eight
// int16 lanes per instruction. Built with -DNNUE_PIE (the cores3 envs);
// nnue.cpp has the plain C++ versions (the reference, and the PC build).
// All pointers must be 16-byte aligned and n a multiple of 8. Adds and
// subtracts saturate (the C++ versions wrap) - equal while nothing
// overflows, which nnue_compare_paths() checks on the device.
#ifdef NNUE_PIE

    .text
    .align 4

// void nn_add_sub(int16_t* dst, const int16_t* src, const int16_t* add,
//                 const int16_t* sub, int n)          dst = src + add - sub
    .align  4
    .global nn_add_sub
    .type   nn_add_sub, @function
nn_add_sub:
    entry   a1, 32
    srli    a6, a6, 3
    loopnez a6, .Ladd_sub_end
    ee.vld.128.ip   q0, a3, 16
    ee.vld.128.ip   q1, a4, 16
    ee.vld.128.ip   q2, a5, 16
    ee.vadds.s16    q0, q0, q1
    ee.vsubs.s16    q0, q0, q2
    ee.vst.128.ip   q0, a2, 16
.Ladd_sub_end:
    retw.n

// void nn_add(int16_t* dst, const int16_t* src, const int16_t* w, int n)
//                                                      dst = src + w
    .align  4
    .global nn_add
    .type   nn_add, @function
nn_add:
    entry   a1, 32
    srli    a5, a5, 3
    loopnez a5, .Ladd_end
    ee.vld.128.ip   q0, a3, 16
    ee.vld.128.ip   q1, a4, 16
    ee.vadds.s16    q0, q0, q1
    ee.vst.128.ip   q0, a2, 16
.Ladd_end:
    retw.n

// void nn_sub(int16_t* dst, const int16_t* src, const int16_t* w, int n)
//                                                      dst = src - w
    .align  4
    .global nn_sub
    .type   nn_sub, @function
nn_sub:
    entry   a1, 32
    srli    a5, a5, 3
    loopnez a5, .Lsub_end
    ee.vld.128.ip   q0, a3, 16
    ee.vld.128.ip   q1, a4, 16
    ee.vsubs.s16    q0, q0, q1
    ee.vst.128.ip   q0, a2, 16
.Lsub_end:
    retw.n

// void nn_crelu(int16_t* dst, const int16_t* src, int n, const int16_t* qa8)
//              dst = clamp(src, 0, QA); qa8 = eight copies of QA
    .align  4
    .global nn_crelu
    .type   nn_crelu, @function
nn_crelu:
    entry   a1, 32
    ee.zero.q       q6
    ee.vld.128.ip   q7, a5, 0
    srli    a4, a4, 3
    loopnez a4, .Lcrelu_end
    ee.vld.128.ip   q0, a3, 16
    ee.vmax.s16     q0, q0, q6
    ee.vmin.s16     q0, q0, q7
    ee.vst.128.ip   q0, a2, 16
.Lcrelu_end:
    retw.n

// int32_t nn_crelu_dot(const int16_t* x, const int16_t* w, int n, const int16_t* qa8)
//              sum of clamp(x, 0, QA) * w
    .align  4
    .global nn_crelu_dot
    .type   nn_crelu_dot, @function
nn_crelu_dot:
    entry   a1, 32
    ee.zero.accx
    ee.zero.q       q6
    ee.vld.128.ip   q7, a5, 0
    srli    a4, a4, 3
    loopnez a4, .Lcrelu_dot_end
    ee.vld.128.ip   q0, a2, 16
    ee.vld.128.ip   q1, a3, 16
    ee.vmax.s16     q0, q0, q6
    ee.vmin.s16     q0, q0, q7
    ee.vmulas.s16.accx q0, q1
.Lcrelu_dot_end:
    rur.accx_0      a2
    retw.n

// int32_t nn_dot(const int16_t* x, const int16_t* w, int n)     sum of x * w
    .align  4
    .global nn_dot
    .type   nn_dot, @function
nn_dot:
    entry   a1, 32
    ee.zero.accx
    srli    a4, a4, 3
    loopnez a4, .Ldot_end
    ee.vld.128.ip   q0, a2, 16
    ee.vld.128.ip   q1, a3, 16
    ee.vmulas.s16.accx q0, q1
.Ldot_end:
    rur.accx_0      a2
    retw.n

// void nn_layer(const int16_t* x, const int16_t* w, const int32_t* b,
//               int16_t* out, int in_n, int out_n)
//   out[j] = clamp((dot(x, w[j]) + b[j]) >> 6, 0, 255), w row-major [out_n][in_n]
//   (NNUE_H_SHIFT = 6, NNUE_QA = 255 - keep in step with nnue.h)
    .align  4
    .global nn_layer
    .type   nn_layer, @function
nn_layer:
    entry   a1, 32
    srli    a6, a6, 3           // input chunks of 8
    movi    a9, 255             // QA
    movi    a12, 0
    beqz    a7, .Llayer_done
.Llayer_row:
    ee.zero.accx
    mov     a8, a2              // x restarts for every row; w runs on
    loopnez a6, .Llayer_dot_end
    ee.vld.128.ip   q0, a8, 16
    ee.vld.128.ip   q1, a3, 16
    ee.vmulas.s16.accx q0, q1
.Llayer_dot_end:
    rur.accx_0      a10
    l32i    a11, a4, 0
    addi    a4, a4, 4
    add     a10, a10, a11
    srai    a10, a10, 6
    max     a10, a10, a12
    min     a10, a10, a9
    s16i    a10, a5, 0
    addi    a5, a5, 2
    addi    a7, a7, -1
    bnez    a7, .Llayer_row
.Llayer_done:
    retw.n

#endif  // NNUE_PIE
