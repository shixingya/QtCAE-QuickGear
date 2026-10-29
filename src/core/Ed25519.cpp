#include "core/Ed25519.h"

#include <QCryptographicHash>
#include <QRandomGenerator>
#include <QtEndian>

#include <cstring>

namespace {

typedef unsigned long long u64;
typedef unsigned __int128 u128;
typedef unsigned char u8;

constexpr u64 kMask51 = (1ULL << 51) - 1;

/** 域元素：5 个 51 位 limb，值 = Σ v[i]·2^(51i)，模 2^255-19 */
struct Fe
{
    u64 v[5];
};

/** 扩展扭曲爱德华坐标：(X:Y:Z:T)，x = X/Z, y = Y/Z, T = XY/Z */
struct GeP3
{
    Fe X, Y, Z, T;
};

/* ------------------------------------------------------------------ */
/* 域运算                                                              */
/* ------------------------------------------------------------------ */

void feCarry(Fe &h)
{
    u64 c;
    c = h.v[0] >> 51; h.v[0] &= kMask51; h.v[1] += c;
    c = h.v[1] >> 51; h.v[1] &= kMask51; h.v[2] += c;
    c = h.v[2] >> 51; h.v[2] &= kMask51; h.v[3] += c;
    c = h.v[3] >> 51; h.v[3] &= kMask51; h.v[4] += c;
    c = h.v[4] >> 51; h.v[4] &= kMask51; h.v[0] += 19 * c;
    // v[0] 刚加了 19c，可能再次越过 2^51，需要第二次进位
    c = h.v[0] >> 51; h.v[0] &= kMask51; h.v[1] += c;
}

Fe feZero()
{
    Fe h;
    h.v[0] = h.v[1] = h.v[2] = h.v[3] = h.v[4] = 0;
    return h;
}

Fe feOne()
{
    Fe h = feZero();
    h.v[0] = 1;
    return h;
}

Fe feAdd(const Fe &a, const Fe &b)
{
    Fe h;
    for (int i = 0; i < 5; ++i)
        h.v[i] = a.v[i] + b.v[i];
    feCarry(h);
    return h;
}

/** a - b，先加 2p 避免下溢 */
Fe feSub(const Fe &a, const Fe &b)
{
    Fe h;
    h.v[0] = a.v[0] + 0xFFFFFFFFFFFDAULL - b.v[0];   // 2·(2^51-19)
    for (int i = 1; i < 5; ++i)
        h.v[i] = a.v[i] + 0xFFFFFFFFFFFFEULL - b.v[i];   // 2·(2^51-1)
    feCarry(h);
    return h;
}

Fe feNeg(const Fe &a)
{
    return feSub(feZero(), a);
}

Fe feMul(const Fe &a, const Fe &b)
{
    const u64 *x = a.v;
    const u64 *y = b.v;

    u128 t0 = (u128)x[0] * y[0]
              + (u128)19 * ((u128)x[1] * y[4] + (u128)x[2] * y[3] + (u128)x[3] * y[2]
                            + (u128)x[4] * y[1]);
    u128 t1 = (u128)x[0] * y[1] + (u128)x[1] * y[0]
              + (u128)19 * ((u128)x[2] * y[4] + (u128)x[3] * y[3] + (u128)x[4] * y[2]);
    u128 t2 = (u128)x[0] * y[2] + (u128)x[1] * y[1] + (u128)x[2] * y[0]
              + (u128)19 * ((u128)x[3] * y[4] + (u128)x[4] * y[3]);
    u128 t3 = (u128)x[0] * y[3] + (u128)x[1] * y[2] + (u128)x[2] * y[1] + (u128)x[3] * y[0]
              + (u128)19 * ((u128)x[4] * y[4]);
    u128 t4 = (u128)x[0] * y[4] + (u128)x[1] * y[3] + (u128)x[2] * y[2] + (u128)x[3] * y[1]
              + (u128)x[4] * y[0];

    Fe h;
    u64  r0 = (u64)t0 & kMask51; t1 += (t0 >> 51);
    u64  r1 = (u64)t1 & kMask51; t2 += (t1 >> 51);
    u64  r2 = (u64)t2 & kMask51; t3 += (t2 >> 51);
    u64  r3 = (u64)t3 & kMask51; t4 += (t3 >> 51);
    u64  r4 = (u64)t4 & kMask51;
    r0 += (u64)(t4 >> 51) * 19;
    const u64 c = r0 >> 51; r0 &= kMask51; r1 += c;

    h.v[0] = r0; h.v[1] = r1; h.v[2] = r2; h.v[3] = r3; h.v[4] = r4;
    return h;
}

Fe feSquare(const Fe &a)
{
    return feMul(a, a);
}

/** 读 32 字节小端（忽略最高位）为域元素 */
Fe feFromBytes(const u8 s[32])
{
    Fe h;
    h.v[0] = qFromLittleEndian<quint64>(s + 0) & kMask51;
    h.v[1] = (qFromLittleEndian<quint64>(s + 6) >> 3) & kMask51;
    h.v[2] = (qFromLittleEndian<quint64>(s + 12) >> 6) & kMask51;
    h.v[3] = (qFromLittleEndian<quint64>(s + 19) >> 1) & kMask51;
    h.v[4] = (qFromLittleEndian<quint64>(s + 24) >> 12) & kMask51;
    return h;
}

/** 写成 32 字节小端（完全约简到 [0, p)） */
void feToBytes(u8 out[32], const Fe &in)
{
    Fe t = in;
    feCarry(t);
    feCarry(t);

    // 判断是否 >= p：p 的 limb 是 (2^51-19, 2^51-1 ×4)
    u64 q = (t.v[0] + 19) >> 51;
    q = (t.v[1] + q) >> 51;
    q = (t.v[2] + q) >> 51;
    q = (t.v[3] + q) >> 51;
    q = (t.v[4] + q) >> 51;

    t.v[0] += 19 * q;
    u64 c;
    c = t.v[0] >> 51; t.v[0] &= kMask51; t.v[1] += c;
    c = t.v[1] >> 51; t.v[1] &= kMask51; t.v[2] += c;
    c = t.v[2] >> 51; t.v[2] &= kMask51; t.v[3] += c;
    c = t.v[3] >> 51; t.v[3] &= kMask51; t.v[4] += c;
    t.v[4] &= kMask51;   // 减掉 p 后不会超过 2^255

    out[0]  = (u8)(t.v[0] >> 0);
    out[1]  = (u8)(t.v[0] >> 8);
    out[2]  = (u8)(t.v[0] >> 16);
    out[3]  = (u8)(t.v[0] >> 24);
    out[4]  = (u8)(t.v[0] >> 32);
    out[5]  = (u8)(t.v[0] >> 40);
    out[6]  = (u8)((t.v[0] >> 48) | (t.v[1] << 3));
    out[7]  = (u8)(t.v[1] >> 5);
    out[8]  = (u8)(t.v[1] >> 13);
    out[9]  = (u8)(t.v[1] >> 21);
    out[10] = (u8)(t.v[1] >> 29);
    out[11] = (u8)(t.v[1] >> 37);
    out[12] = (u8)((t.v[1] >> 45) | (t.v[2] << 6));
    out[13] = (u8)(t.v[2] >> 2);
    out[14] = (u8)(t.v[2] >> 10);
    out[15] = (u8)(t.v[2] >> 18);
    out[16] = (u8)(t.v[2] >> 26);
    out[17] = (u8)(t.v[2] >> 34);
    out[18] = (u8)(t.v[2] >> 42);
    out[19] = (u8)((t.v[2] >> 50) | (t.v[3] << 1));
    out[20] = (u8)(t.v[3] >> 7);
    out[21] = (u8)(t.v[3] >> 15);
    out[22] = (u8)(t.v[3] >> 23);
    out[23] = (u8)(t.v[3] >> 31);
    out[24] = (u8)(t.v[3] >> 39);
    out[25] = (u8)((t.v[3] >> 47) | (t.v[4] << 4));
    out[26] = (u8)(t.v[4] >> 4);
    out[27] = (u8)(t.v[4] >> 12);
    out[28] = (u8)(t.v[4] >> 20);
    out[29] = (u8)(t.v[4] >> 28);
    out[30] = (u8)(t.v[4] >> 36);
    out[31] = (u8)(t.v[4] >> 44);
}

bool feIsNegative(const Fe &a)
{
    u8 bytes[32];
    feToBytes(bytes, a);
    return (bytes[0] & 1) != 0;
}

bool feIsNonZero(const Fe &a)
{
    u8 bytes[32];
    feToBytes(bytes, a);
    u8 acc = 0;
    for (int i = 0; i < 32; ++i)
        acc |= bytes[i];
    return acc != 0;
}

/** z^exp，exp 为 32 字节小端；从最高位往下平方-乘 */
Fe fePow(const Fe &z, const u8 exp[32])
{
    Fe r = feOne();
    for (int bit = 255; bit >= 0; --bit) {
        r = feSquare(r);
        if ((exp[bit >> 3] >> (bit & 7)) & 1)
            r = feMul(r, z);
    }
    return r;
}

/** 构造 32 字节指数：低字节 low、高字节 high、中间全 0xFF */
void buildExp(u8 out[32], u8 low, u8 high)
{
    std::memset(out, 0xFF, 32);
    out[0] = low;
    out[31] = high;
}

Fe feInvert(const Fe &z)
{
    u8 exp[32];
    buildExp(exp, 0xEB, 0x7F);   // p-2 = 2^255-21
    return fePow(z, exp);
}

/* ------------------------------------------------------------------ */
/* 曲线常量（惰性计算，避免手抄大数出错）                              */
/* ------------------------------------------------------------------ */

const Fe &curveD()
{
    static const Fe d = [] {
        // d = -121665 / 121666
        Fe num = feZero();
        num.v[0] = 121665;
        Fe den = feZero();
        den.v[0] = 121666;
        return feMul(feNeg(num), feInvert(den));
    }();
    return d;
}

const Fe &curveD2()
{
    static const Fe d2 = [] {
        const Fe d = curveD();
        return feAdd(d, d);
    }();
    return d2;
}

const Fe &sqrtMinusOne()
{
    static const Fe s = [] {
        Fe two = feZero();
        two.v[0] = 2;
        u8 exp[32];
        buildExp(exp, 0xFB, 0x1F);   // (p-1)/4 = 2^253-5
        return fePow(two, exp);
    }();
    return s;
}

/* ------------------------------------------------------------------ */
/* 点运算                                                              */
/* ------------------------------------------------------------------ */

GeP3 geIdentity()
{
    GeP3 p;
    p.X = feZero();
    p.Y = feOne();
    p.Z = feOne();
    p.T = feZero();
    return p;
}

/** add-2008-hwcd-3，a = -1 的统一加法公式 */
GeP3 geAdd(const GeP3 &p, const GeP3 &q)
{
    const Fe A = feMul(feSub(p.Y, p.X), feSub(q.Y, q.X));
    const Fe B = feMul(feAdd(p.Y, p.X), feAdd(q.Y, q.X));
    const Fe C = feMul(feMul(p.T, q.T), curveD2());
    const Fe D = feMul(feAdd(p.Z, p.Z), q.Z);
    const Fe E = feSub(B, A);
    const Fe F = feSub(D, C);
    const Fe G = feAdd(D, C);
    const Fe H = feAdd(B, A);

    GeP3 r;
    r.X = feMul(E, F);
    r.Y = feMul(G, H);
    r.T = feMul(E, H);
    r.Z = feMul(F, G);
    return r;
}

/** dbl-2008-hwcd，a = -1 */
GeP3 geDouble(const GeP3 &p)
{
    const Fe A = feSquare(p.X);
    const Fe B = feSquare(p.Y);
    const Fe C = feAdd(feSquare(p.Z), feSquare(p.Z));
    const Fe D = feNeg(A);
    const Fe E = feSub(feSub(feSquare(feAdd(p.X, p.Y)), A), B);
    const Fe G = feAdd(D, B);
    const Fe F = feSub(G, C);
    const Fe H = feSub(D, B);

    GeP3 r;
    r.X = feMul(E, F);
    r.Y = feMul(G, H);
    r.T = feMul(E, H);
    r.Z = feMul(F, G);
    return r;
}

GeP3 geNegate(const GeP3 &p)
{
    GeP3 r;
    r.X = feNeg(p.X);
    r.Y = p.Y;
    r.Z = p.Z;
    r.T = feNeg(p.T);
    return r;
}

/** [s]P，双倍-加法；变时间，代价是私钥签发时的侧信道（不对外暴露） */
GeP3 geScalarMult(const u8 s[32], const GeP3 &p)
{
    GeP3 r = geIdentity();
    for (int bit = 255; bit >= 0; --bit) {
        r = geDouble(r);
        if ((s[bit >> 3] >> (bit & 7)) & 1)
            r = geAdd(r, p);
    }
    return r;
}

void geToBytes(u8 out[32], const GeP3 &p)
{
    const Fe zinv = feInvert(p.Z);
    const Fe x    = feMul(p.X, zinv);
    const Fe y    = feMul(p.Y, zinv);
    feToBytes(out, y);
    out[31] ^= (u8)(feIsNegative(x) ? 0x80 : 0x00);
}

/** 解码点；非规范 y、x=0 且 sign=1、不在曲线上，一律拒绝 */
bool geFromBytes(GeP3 *p, const u8 s[32])
{
    u8 ybytes[32];
    std::memcpy(ybytes, s, 32);
    const int sign = ybytes[31] >> 7;
    ybytes[31] &= 0x7F;

    const Fe y = feFromBytes(ybytes);

    // 非规范编码：把 y 写回去必须还是原来那 31 个半字节
    u8 reencoded[32];
    feToBytes(reencoded, y);
    if (std::memcmp(reencoded, ybytes, 32) != 0)
        return false;

    const Fe one = feOne();
    const Fe u   = feSub(feSquare(y), one);                 // y²-1
    const Fe v   = feAdd(feMul(feSquare(y), curveD()), one); // d·y²+1

    const Fe v3  = feMul(feSquare(v), v);                   // v³
    const Fe uv7 = feMul(feMul(feSquare(v3), v), u);        // u·v⁷

    u8 expP58[32];
    buildExp(expP58, 0xFD, 0x0F);   // (p-5)/8 = 2^252-3

    Fe x = fePow(uv7, expP58);
    x = feMul(feMul(x, v3), u);                             // u·v³·(u·v⁷)^((p-5)/8)

    const Fe vxx   = feMul(feSquare(x), v);
    const Fe check = feSub(vxx, u);
    if (feIsNonZero(check)) {
        const Fe check2 = feAdd(vxx, u);
        if (feIsNonZero(check2))
            return false;
        x = feMul(x, sqrtMinusOne());
    }

    if (feIsNegative(x) != (sign != 0))
        x = feNeg(x);
    if (!feIsNonZero(x) && sign != 0)
        return false;

    p->X = x;
    p->Y = y;
    p->Z = feOne();
    p->T = feMul(x, y);
    return true;
}

/** 基点 B = (x, 4/5)：用解码得到，省掉手抄 32 字节坐标 */
const GeP3 &basePoint()
{
    static const GeP3 b = [] {
        u8 encoded[32];
        std::memset(encoded, 0x66, 32);
        encoded[0]  = 0x58;
        encoded[31] = 0x66;
        GeP3 p;
        geFromBytes(&p, encoded);
        return p;
    }();
    return b;
}

/* ------------------------------------------------------------------ */
/* 标量运算（模 L）                                                    */
/* ------------------------------------------------------------------ */

// L = 2^252 + 27742317777372353535851937790883648493
const u64 kL[5] = {0x5812631a5cf5d3edULL, 0x14def9dea2f79cd6ULL, 0ULL,
                   0x1000000000000000ULL, 0ULL};

void loadScalar(u64 out[4], const u8 in[32])
{
    for (int i = 0; i < 4; ++i)
        out[i] = qFromLittleEndian<quint64>(in + i * 8);
}

/** 64 字节小端 → 32 字节 mod L（逐位移入 + 条件减法，够用且不烧脑） */
void scReduce(u8 out[32], const u8 in[64])
{
    u64 r[5] = {0, 0, 0, 0, 0};
    for (int bit = 511; bit >= 0; --bit) {
        u64 carry = (u64)((in[bit >> 3] >> (bit & 7)) & 1);
        for (int j = 0; j < 5; ++j) {
            const u64 next = r[j] >> 63;
            r[j] = (r[j] << 1) | carry;
            carry = next;
        }

        bool ge = false;
        for (int j = 4; j >= 0; --j) {
            if (r[j] != kL[j]) {
                ge = r[j] > kL[j];
                break;
            }
        }
        if (ge) {
            u64 borrow = 0;
            for (int j = 0; j < 5; ++j) {
                const u64 sub = kL[j] + borrow;
                borrow = (r[j] < sub) ? 1 : 0;
                r[j] -= sub;
            }
        }
    }

    for (int j = 0; j < 4; ++j)
        qToLittleEndian<quint64>(r[j], out + j * 8);
}

/** (a·b + c) mod L */
void scMulAdd(u8 out[32], const u8 a[32], const u8 b[32], const u8 c[32])
{
    u64 aL[4], bL[4], cL[4];
    loadScalar(aL, a);
    loadScalar(bL, b);
    loadScalar(cL, c);

    u64 t[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};

    for (int i = 0; i < 4; ++i) {
        u128 carry = 0;
        for (int j = 0; j < 4; ++j) {
            const u128 cur = (u128)aL[i] * bL[j] + t[i + j] + carry;
            t[i + j]      = (u64)cur;
            carry         = cur >> 64;
        }
        int k = i + 4;
        while (carry != 0) {
            const u128 cur = (u128)t[k] + carry;
            t[k]           = (u64)cur;
            carry          = cur >> 64;
            ++k;
        }
    }

    u128 carry = 0;
    for (int k = 0; k < 4; ++k) {
        const u128 cur = (u128)t[k] + cL[k] + carry;
        t[k]           = (u64)cur;
        carry          = cur >> 64;
    }
    int k = 4;
    while (carry != 0) {
        const u128 cur = (u128)t[k] + carry;
        t[k]           = (u64)cur;
        carry          = cur >> 64;
        ++k;
    }

    u8 wide[64];
    for (int i = 0; i < 8; ++i)
        qToLittleEndian<quint64>(t[i], wide + i * 8);
    scReduce(out, wide);
}

/** S 必须 < L，否则签名可被替换成 S+L 之类的变体 */
bool scIsCanonical(const u8 s[32])
{
    u64 v[4];
    loadScalar(v, s);
    for (int j = 3; j >= 0; --j) {
        if (v[j] != kL[j])
            return v[j] < kL[j];
    }
    return false;   // 恰好等于 L 也不行
}

QByteArray sha512(const QByteArray &data)
{
    return QCryptographicHash::hash(data, QCryptographicHash::Sha512);
}

} // namespace

namespace Ed25519
{

QByteArray randomSeed()
{
    QByteArray seed(32, Qt::Uninitialized);
    auto *rng = QRandomGenerator::system();
    for (int i = 0; i < 32; i += 4)
        qToLittleEndian<quint32>(rng->generate(), reinterpret_cast<uchar *>(seed.data()) + i);
    return seed;
}

QString encodeKey(const QByteArray &key)
{
    return QString::fromLatin1(key.toBase64());
}

QByteArray decodeKey(const QString &text, QString *error)
{
    const QByteArray raw = QByteArray::fromBase64(text.trimmed().toLatin1());
    if (raw.size() != 32) {
        if (error) {
            *error = QStringLiteral("密钥应为 32 字节的 base64，实际解出 %1 字节")
                         .arg(raw.size());
        }
        return QByteArray();
    }
    return raw;
}

bool derivePublicKey(const QByteArray &seed, QByteArray *publicKey, QString *error)
{
    if (seed.size() != 32) {
        if (error) *error = QStringLiteral("种子必须是 32 字节，实际 %1").arg(seed.size());
        return false;
    }

    const QByteArray h = sha512(seed);
    u8 a[32];
    std::memcpy(a, h.constData(), 32);
    a[0] &= 248;
    a[31] &= 127;
    a[31] |= 64;

    const GeP3 A = geScalarMult(a, basePoint());
    u8 pk[32];
    geToBytes(pk, A);
    *publicKey = QByteArray(reinterpret_cast<const char *>(pk), 32);
    return true;
}

bool sign(const QByteArray &message, const QByteArray &seed, QByteArray *signature,
          QString *error)
{
    if (seed.size() != 32) {
        if (error) *error = QStringLiteral("种子必须是 32 字节，实际 %1").arg(seed.size());
        return false;
    }

    QByteArray publicKey;
    if (!derivePublicKey(seed, &publicKey, error))
        return false;

    const QByteArray h = sha512(seed);
    u8 a[32];
    std::memcpy(a, h.constData(), 32);
    a[0] &= 248;
    a[31] &= 127;
    a[31] |= 64;
    const QByteArray prefix = h.mid(32, 32);

    u8 r[32];
    {
        const QByteArray wide = sha512(prefix + message);
        scReduce(r, reinterpret_cast<const u8 *>(wide.constData()));
    }

    const GeP3 R = geScalarMult(r, basePoint());
    u8 Renc[32];
    geToBytes(Renc, R);

    u8 k[32];
    {
        QByteArray kInput(reinterpret_cast<const char *>(Renc), 32);
        kInput.append(publicKey);
        kInput.append(message);
        const QByteArray wide = sha512(kInput);
        scReduce(k, reinterpret_cast<const u8 *>(wide.constData()));
    }

    u8 S[32];
    scMulAdd(S, k, a, r);

    QByteArray sig(64, Qt::Uninitialized);
    std::memcpy(sig.data(), Renc, 32);
    std::memcpy(sig.data() + 32, S, 32);
    *signature = sig;
    return true;
}

bool verify(const QByteArray &message, const QByteArray &signature, const QByteArray &publicKey)
{
    if (signature.size() != 64 || publicKey.size() != 32)
        return false;

    const u8 *sig = reinterpret_cast<const u8 *>(signature.constData());
    const u8 *pk  = reinterpret_cast<const u8 *>(publicKey.constData());

    if (!scIsCanonical(sig + 32))
        return false;

    GeP3 A;
    if (!geFromBytes(&A, pk))
        return false;

    GeP3 R;
    if (!geFromBytes(&R, sig))
        return false;

    u8 h[32];
    {
        QByteArray kInput(reinterpret_cast<const char *>(sig), 32);
        kInput.append(publicKey);
        kInput.append(message);
        const QByteArray wide = sha512(kInput);
        scReduce(h, reinterpret_cast<const u8 *>(wide.constData()));
    }

    const GeP3 sB  = geScalarMult(sig + 32, basePoint());
    const GeP3 hA  = geScalarMult(h, A);
    const GeP3 sum = geAdd(sB, geNegate(hA));

    u8 encoded[32];
    geToBytes(encoded, sum);
    return std::memcmp(encoded, sig, 32) == 0;
}

} // namespace Ed25519
