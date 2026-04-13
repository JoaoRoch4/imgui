#include "test_engine_layer.hpp"
#include "imgui_te_context.h"
#include "imgui_te_engine.h"
#include "imgui_te_ui.h"

TestEngineLayer::TestEngineLayer()
    : Engine { nullptr }
    , ShowWindow { true }
{
}

void TestEngineLayer::Init()
{
	Engine = ImGuiTestEngine_CreateContext();

	ImGuiTestEngineIO& test_io = ImGuiTestEngine_GetIO(Engine);
	test_io.ConfigVerboseLevel = ImGuiTestVerboseLevel_Info;
	test_io.ConfigVerboseLevelOnError = ImGuiTestVerboseLevel_Debug;
	test_io.ConfigRunSpeed = ImGuiTestRunSpeed_Fast;

	ImGuiTestEngine_Start(Engine, ImGui::GetCurrentContext());
	RegisterTests();
}

void TestEngineLayer::Stop()
{
    ImGuiTestEngine_Stop(Engine);
}

void TestEngineLayer::Shutdown()
{
	ImGuiTestEngine_DestroyContext(Engine);
	Engine = nullptr;
}

void TestEngineLayer::PostSwap() { ImGuiTestEngine_PostSwap(Engine); }

void TestEngineLayer::BuildUI() { ImGuiTestEngine_ShowTestEngineWindows(Engine, &ShowWindow); }

void TestEngineLayer::RegisterTests()
{
	// Click the counter button in "Hello, world!" and verify the window is open.
	ImGuiTest* t = IM_REGISTER_TEST(Engine, "app", "hello_button_click");
	t->TestFunc = [](ImGuiTestContext* ctx) {
		ctx->SetRef("Hello, world!");
		ctx->WindowFocus("");
		ctx->ItemClick("Button");
		ctx->ItemClick("Button");
		ctx->ItemClick("Button");
	};

	// Toggle the Demo Window checkbox off and back on.
	ImGuiTest* t2 = IM_REGISTER_TEST(Engine, "app", "toggle_demo_window");
	t2->TestFunc = [](ImGuiTestContext* ctx) {
		ctx->SetRef("Hello, world!");
		ctx->ItemUncheck("Demo Window");
		ctx->ItemCheck("Demo Window");
	};
}
