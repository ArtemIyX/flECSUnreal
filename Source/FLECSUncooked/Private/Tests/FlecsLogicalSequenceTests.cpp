#if WITH_DEV_AUTOMATION_TESTS
#include "Diagnostics/FlecsLogicalSequence.h"
#include "Misc/AutomationTest.h"
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlecsLogicalSequenceTest, "Flecs.Diagnostics.LogicalSequence", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FFlecsLogicalSequenceTest::RunTest(const FString& Parameters)
{
 (void)Parameters;
 FFlecsLogicalSequence Sequence;
 TestFalse(TEXT("Zero epoch rejected"), Sequence.Accept(0, 1));
 TestTrue(TEXT("First sequence accepted"), Sequence.Accept(1, 1));
 TestFalse(TEXT("Retry ignored"), Sequence.Accept(1, 1));
 TestTrue(TEXT("Next sequence accepted"), Sequence.Accept(1, 2));
 TestFalse(TEXT("Old retry ignored"), Sequence.Accept(1, 1));
 TestTrue(TEXT("New epoch resets sequence"), Sequence.Accept(2, 1));
 FFlecsLogicalSequence Wrap; Wrap.Epoch = 2; Wrap.LastSequence = MAX_uint32;
 TestTrue(TEXT("Sequence wrap accepted"), Wrap.Accept(2, 1));
 return true;
}
#endif
