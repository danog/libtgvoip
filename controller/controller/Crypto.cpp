#include "../PrivateDefines.cpp"

using namespace tgvoip;
using namespace std;

size_t VoIPController::decryptPacket(unsigned char *buffer, BufferInputStream &in)
{
    bool retryWith2 = false;
    size_t innerLen = 0;
    size_t offset = in.GetOffset();
    bool shortFormat = peerVersion >= 8 || (!peerVersion && connectionMaxLayer >= 92);

    if (!useMTProto2)
    {
        unsigned char fingerprint[8], msgHash[16];
        in.ReadBytes(fingerprint, 8);
        in.ReadBytes(msgHash, 16);
        unsigned char key[32], iv[32];
        KDF(msgHash, isOutgoing ? 8 : 0, key, iv);
        unsigned char aesOut[MSC_STACK_FALLBACK(in.Remaining(), 1500)];
        if (in.Remaining() > sizeof(aesOut))
            return 0;
        crypto.aes_ige_decrypt((unsigned char *)buffer + in.GetOffset(), aesOut, in.Remaining(), key, iv);
        BufferInputStream _in(aesOut, in.Remaining());
        unsigned char sha[SHA1_LENGTH];
        uint32_t _len = _in.ReadUInt32();
        if (_len > _in.Remaining())
            _len = (uint32_t)_in.Remaining();
        crypto.sha1((uint8_t *)(aesOut), (size_t)(_len + 4), sha);
        if (memcmp(msgHash, sha + (SHA1_LENGTH - 16), 16) != 0)
        {
            LOGW("Received packet has wrong hash after decryption");
            if (state == STATE_WAIT_INIT || state == STATE_WAIT_INIT_ACK)
                retryWith2 = true;
            else
                return 0;
        }
        else
        {
            memcpy(buffer + in.GetOffset(), aesOut, in.Remaining());
            in.ReadInt32();
        }
    }

    if (useMTProto2 || retryWith2)
    {
        in.Seek(offset); // peer tag

        unsigned char fingerprint[8], msgKey[16];
        if (!shortFormat)
        {
            in.ReadBytes(fingerprint, 8);
            if (memcmp(fingerprint, keyFingerprint, 8) != 0)
            {
                LOGW("Received packet has wrong key fingerprint");
                return 0;
            }
        }
        in.ReadBytes(msgKey, 16);

        unsigned char decrypted[1500];
        unsigned char aesKey[32], aesIv[32];
        KDF2(msgKey, isOutgoing ? 8 : 0, aesKey, aesIv);
        size_t decryptedLen = in.Remaining();
        if (decryptedLen > sizeof(decrypted))
            return 0;
        if (decryptedLen % 16 != 0)
        {
            LOGW("wrong decrypted length");
            return 0;
        }

        crypto.aes_ige_decrypt(buffer + in.GetOffset(), decrypted, decryptedLen, aesKey, aesIv);

        in = BufferInputStream(decrypted, decryptedLen);
        //LOGD("received packet length: %d", in.ReadInt32());
        size_t sizeSize = shortFormat ? 0 : 4;

        BufferOutputStream buf(decryptedLen + 32);
        size_t x = isOutgoing ? 8 : 0;
        buf.WriteBytes(encryptionKey + 88 + x, 32);
        buf.WriteBytes(decrypted + sizeSize, decryptedLen - sizeSize);
        unsigned char msgKeyLarge[32];
        crypto.sha256(buf.GetBuffer(), buf.GetLength(), msgKeyLarge);

        if (memcmp(msgKey, msgKeyLarge + 8, 16) != 0)
        {
            LOGW("Received packet has wrong hash");
            return 0;
        }

        innerLen = static_cast<uint32_t>(shortFormat ? in.ReadInt16() : in.ReadInt32());
        if (innerLen > decryptedLen - sizeSize)
        {
            LOGW("Received packet has wrong inner length (%d with total of %u)", (int)innerLen, (unsigned int)decryptedLen);
            return 0;
        }
        if (decryptedLen - innerLen < (shortFormat ? 16 : 12))
        {
            LOGW("Received packet has too little padding (%u)", (unsigned int)(decryptedLen - innerLen));
            return 0;
        }
        memcpy(buffer, decrypted + (shortFormat ? 2 : 4), innerLen);
        in = BufferInputStream(buffer, innerLen);
        if (retryWith2)
        {
            LOGD("Successfully decrypted packet in MTProto2.0 fallback, upgrading");
            useMTProto2 = true;
        }
    }

    return innerLen;
}

void VoIPController::SetEncryptionKey(std::vector<uint8_t> key, bool isOutgoing)
{
    memcpy(encryptionKey, key.data(), 256);
    uint8_t sha1[SHA1_LENGTH];
    crypto.sha1((uint8_t *)encryptionKey, 256, sha1);
    memcpy(keyFingerprint, sha1 + (SHA1_LENGTH - 8), 8);
    uint8_t sha256[SHA256_LENGTH];
    crypto.sha256((uint8_t *)encryptionKey, 256, sha256);
    memcpy(callID, sha256 + (SHA256_LENGTH - 16), 16);
    this->isOutgoing = isOutgoing;
}

void VoIPController::KDF(unsigned char *msgKey, size_t x, unsigned char *aesKey, unsigned char *aesIv)
{
    uint8_t sA[SHA1_LENGTH], sB[SHA1_LENGTH], sC[SHA1_LENGTH], sD[SHA1_LENGTH];
    BufferOutputStream buf(128);
    buf.WriteBytes(msgKey, 16);
    buf.WriteBytes(encryptionKey + x, 32);
    crypto.sha1(buf.GetBuffer(), buf.GetLength(), sA);
    buf.Reset();
    buf.WriteBytes(encryptionKey + 32 + x, 16);
    buf.WriteBytes(msgKey, 16);
    buf.WriteBytes(encryptionKey + 48 + x, 16);
    crypto.sha1(buf.GetBuffer(), buf.GetLength(), sB);
    buf.Reset();
    buf.WriteBytes(encryptionKey + 64 + x, 32);
    buf.WriteBytes(msgKey, 16);
    crypto.sha1(buf.GetBuffer(), buf.GetLength(), sC);
    buf.Reset();
    buf.WriteBytes(msgKey, 16);
    buf.WriteBytes(encryptionKey + 96 + x, 32);
    crypto.sha1(buf.GetBuffer(), buf.GetLength(), sD);
    buf.Reset();
    buf.WriteBytes(sA, 8);
    buf.WriteBytes(sB + 8, 12);
    buf.WriteBytes(sC + 4, 12);
    assert(buf.GetLength() == 32);
    memcpy(aesKey, buf.GetBuffer(), 32);
    buf.Reset();
    buf.WriteBytes(sA + 8, 12);
    buf.WriteBytes(sB, 8);
    buf.WriteBytes(sC + 16, 4);
    buf.WriteBytes(sD, 8);
    assert(buf.GetLength() == 32);
    memcpy(aesIv, buf.GetBuffer(), 32);
}

void VoIPController::KDF2(unsigned char *msgKey, size_t x, unsigned char *aesKey, unsigned char *aesIv)
{
    uint8_t sA[32], sB[32];
    BufferOutputStream buf(128);
    buf.WriteBytes(msgKey, 16);
    buf.WriteBytes(encryptionKey + x, 36);
    crypto.sha256(buf.GetBuffer(), buf.GetLength(), sA);
    buf.Reset();
    buf.WriteBytes(encryptionKey + 40 + x, 36);
    buf.WriteBytes(msgKey, 16);
    crypto.sha256(buf.GetBuffer(), buf.GetLength(), sB);
    buf.Reset();
    buf.WriteBytes(sA, 8);
    buf.WriteBytes(sB + 8, 16);
    buf.WriteBytes(sA + 24, 8);
    memcpy(aesKey, buf.GetBuffer(), 32);
    buf.Reset();
    buf.WriteBytes(sB, 8);
    buf.WriteBytes(sA + 8, 16);
    buf.WriteBytes(sB + 24, 8);
    memcpy(aesIv, buf.GetBuffer(), 32);
}