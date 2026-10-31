/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NavTelemetryRules.h"

#include <gtest/gtest.h>

using namespace NavTelemetryRules;

TEST(NavTelemetryRules, FingerprintIsSixteenHexAndStable)
{
    std::string const a = Fingerprint({{"AiPlayerbot.CoaX", "1"}, {"AiPlayerbot.CoaY", "2"}});
    EXPECT_EQ(a.size(), 16u);
    EXPECT_EQ(a.find_first_not_of("0123456789abcdef"), std::string::npos);
    EXPECT_EQ(a, Fingerprint({{"AiPlayerbot.CoaX", "1"}, {"AiPlayerbot.CoaY", "2"}}));
}

TEST(NavTelemetryRules, FingerprintIgnoresOrderButNotValues)
{
    EXPECT_EQ(Fingerprint({{"A", "1"}, {"B", "2"}}), Fingerprint({{"B", "2"}, {"A", "1"}}));
    EXPECT_NE(Fingerprint({{"A", "1"}, {"B", "2"}}), Fingerprint({{"A", "1"}, {"B", "3"}}));
    EXPECT_NE(Fingerprint({{"A", "1"}}), Fingerprint({{"A", "1"}, {"B", ""}}));
}

TEST(NavTelemetryRules, FingerprintSkipsMetricsOptions)
{
    EXPECT_EQ(Fingerprint({{"AiPlayerbot.CoaX", "1"}, {"AiPlayerbot.CoaMovementMetricsSeconds", "60"}}),
              Fingerprint({{"AiPlayerbot.CoaX", "1"}, {"AiPlayerbot.CoaMovementMetricsSeconds", "15"}}));
}

TEST(NavTelemetryRules, FingerprintSkipsTheBuildInfoOption)
{
    EXPECT_EQ(Fingerprint({{"AiPlayerbot.CoaX", "1"}, {"AiPlayerbot.CoaBuildInfoFile", "/a/b.txt"}}),
              Fingerprint({{"AiPlayerbot.CoaX", "1"}}));
}

TEST(NavTelemetryRules, FingerprintSkipsLoggingAndCosmeticOptions)
{
    std::string const base = Fingerprint({{"AiPlayerbot.CoaSmoothPaths", "1"}});
    for (char const* key : {"AiPlayerbot.CoaGroupTelemetry", "AiPlayerbot.CoaGroupTelemetryRaids",
                            "AiPlayerbot.CoaLogTargets", "AiPlayerbot.CoaStatusFile",
                            "AiPlayerbot.CoaStatusIntervalSeconds", "AiPlayerbot.CoaBotSurname",
                            "AiPlayerbot.CoaLfgAnnounceText", "AiPlayerbot.CoaLfgAnnounceMinutes"})
        EXPECT_EQ(Fingerprint({{"AiPlayerbot.CoaSmoothPaths", "1"}, {key, "7"}}), base) << key;
}

TEST(NavTelemetryRules, FingerprintStillCountsMovementOptions)
{
    EXPECT_NE(Fingerprint({{"AiPlayerbot.CoaSmoothPaths", "1"}}), Fingerprint({{"AiPlayerbot.CoaSmoothPaths", "0"}}));
    EXPECT_NE(Fingerprint({{"AiPlayerbot.CoaRoadShare", "100"}}), Fingerprint({{"AiPlayerbot.CoaRoadShare", "50"}}));
}

TEST(NavTelemetryRules, FingerprintOfNothingIsTheFnvOffset)
{
    EXPECT_EQ(Fingerprint({}), "cbf29ce484222325");
}

TEST(NavTelemetryRules, ParseBuildInfoReadsShaAndSubject)
{
    BuildInfo const info =
        ParseBuildInfo("sha=712dd474fcf1b6d2f63f10bb9990dc1127d86bce\nsubject=CoA movement: pauses\n");
    EXPECT_EQ(info.sha, "712dd474fcf1b6d2f63f10bb9990dc1127d86bce");
    EXPECT_EQ(info.subject, "CoA movement: pauses");
}

TEST(NavTelemetryRules, ParseBuildInfoToleratesCrlfAndMissingParts)
{
    BuildInfo const info = ParseBuildInfo("sha=abc1234\r\n");
    EXPECT_EQ(info.sha, "abc1234");
    EXPECT_EQ(info.subject, "");
}

TEST(NavTelemetryRules, ParseBuildInfoRejectsABadSha)
{
    EXPECT_EQ(ParseBuildInfo("sha=ABC1234\n").sha, "unknown");
    EXPECT_EQ(ParseBuildInfo("sha=abc12\n").sha, "unknown");
    EXPECT_EQ(ParseBuildInfo("sha=xyz1234567\n").sha, "unknown");
    EXPECT_EQ(ParseBuildInfo("").sha, "unknown");
    EXPECT_EQ(ParseBuildInfo("garbage").sha, "unknown");
}

TEST(NavTelemetryRules, FormatBuildLine)
{
    EXPECT_EQ(FormatBuildLine({"abc1234", "fix the thing"}, "0123456789abcdef"),
              "coa build sha=abc1234 cfg=0123456789abcdef subject=\"fix the thing\"");
}

TEST(NavTelemetryRules, FormatBuildLineSanitisesTheSubject)
{
    std::string const line = FormatBuildLine({"abc1234", "say \"hi\"\n\tthere   now"}, "00ff");
    EXPECT_EQ(line, "coa build sha=abc1234 cfg=00ff subject=\"say 'hi' there now\"");
}

TEST(NavTelemetryRules, FormatBuildLineCutsLongSubjectsOnACharacterBoundary)
{
    std::string subject(119, 'a');
    subject += "\xC3\xA9";
    subject += "tail";
    std::string const line = FormatBuildLine({"abc1234", subject}, "00ff");
    std::string const expected = "coa build sha=abc1234 cfg=00ff subject=\"" + std::string(119, 'a') + "\"";
    EXPECT_EQ(line, expected);
}
