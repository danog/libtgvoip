namespace tgvoip
{
// Handle nack window
class Nack
{
public:
    Nack() = default;
    virtual ~Nack() = default;

    // Do not provide seq, as there is a direct correlation between seqs and timestamps (in protocol >= 10)
    void HandleJitterInput(uint32_t timestamp);
    void HandleJitterOutput(uint32_t timestamp);

private:

};
} // namespace tgvoip