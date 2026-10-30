struct failure {};
constexpr int risky(bool fail) { if (fail) { throw failure{}; } return 1; }
constexpr int guarded() { try { return risky(true); } catch (failure) { return 2; } }
static_assert(guarded() == 2);
int main() {}
