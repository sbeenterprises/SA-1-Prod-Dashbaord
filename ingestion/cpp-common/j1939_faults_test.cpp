// Unit tests for j1939_faults.hpp: the Mercury SPN / FMI table, the fault
// wording with and without Mercury names, and the ENGINE_DTC_TEXT enrichment.
// Build: g++ -std=c++17 -o j1939_faults_test j1939_faults_test.cpp && ./j1939_faults_test
#include "j1939_faults.hpp"

#include <cstdio>
#include <string>

static int fails = 0, checks = 0;
#define CHECK(cond) do { ++checks; if (!(cond)) { ++fails; \
    std::printf("  FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); } } while (0)

int main() {
    using namespace usv::j1939;
    size_t n = 0;
    MercuryFaults(&n);
    CHECK(n == 61);                                   // 65 table rows, 61 distinct pairs

    const MercuryFault* f = FindMercuryFault(100, 18);
    CHECK(f && std::string(f->code) == "J143" && std::string(f->name) == "Oil_Press_Low");
    f = FindMercuryFault(190, 0);
    CHECK(f && std::string(f->code) == "J234" && std::string(f->name) == "Engine_Overspeed");
    f = FindMercuryFault(190, 2);                     // listed three times in the table
    CHECK(f && std::string(f->code) == "J115/J689/J2321");
    f = FindMercuryFault(520197, 4);
    CHECK(f && std::string(f->code) == "J2186");
    CHECK(FindMercuryFault(100, 5) == nullptr);        // pair not in the table
    CHECK(FindMercuryFault(9, 2) == nullptr);

    CHECK(FaultText(100, 18, 1, true) == "SPN 100 FMI 18 J143 Oil_Press_Low (x1)");
    CHECK(FaultText(100, 18, 1, false) == "SPN 100 FMI 18 below normal, moderately severe (x1)");
    CHECK(FaultText(100, 5, 2, true) == "SPN 100 FMI 5 current low / open circuit (x2)");   // fallback
    CHECK(FaultText(9, 31, 1, true) == "SPN 9 FMI 31 condition exists (x1)");

    CHECK(EnrichDtcText("NONE", true) == "NONE");
    CHECK(EnrichDtcText("N/A", true) == "N/A");
    CHECK(EnrichDtcText("SPN 100 FMI 18 (x1); SPN 9 FMI 2 (x3)", true)
          == "SPN 100 FMI 18 J143 Oil_Press_Low (x1); SPN 9 FMI 2 erratic or incorrect data (x3)");
    CHECK(EnrichDtcText("SPN 100 FMI 18 (x1)", false)
          == "SPN 100 FMI 18 below normal, moderately severe (x1)");
    CHECK(EnrichDtcText("SPN 190 FMI 0", true) == "SPN 190 FMI 0 J234 Engine_Overspeed");
    // Already named: unchanged, so a text can pass through twice.
    const std::string named = "SPN 100 FMI 18 J143 Oil_Press_Low (x1); SPN 9 FMI 2 erratic or incorrect data (x3)";
    CHECK(EnrichDtcText(named, true) == named);
    CHECK(EnrichDtcText("", true) == "");

    std::printf("j1939_faults_test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
