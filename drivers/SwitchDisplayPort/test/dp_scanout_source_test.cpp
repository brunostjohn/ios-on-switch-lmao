// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "bazel/testing/source_text.h"

using switch_ios::test::Block;
using switch_ios::test::SourceText;

TEST(ExternalOwnership, FrontReadLivesUntilReplacementAndGenerationChangesRefuseReuse)
{
    const auto source = SourceText("../SwitchExternalScanout.cpp");
    const auto latch = Block(source, "bool SwitchExternalScreen::frameEnd()");
    EXPECT_TRUE(latch.has("dcb->windowLatch(latchedAddr)"));
    EXPECT_TRUE(latch.has("front != drawing"));
    EXPECT_TRUE(latch.has("releaseRead(static_cast<unsigned>(front));"));
    EXPECT_TRUE(latch.has("switch_native_scanout_deliver(&slots[front].read);"));
    EXPECT_FALSE(latch.has("switch_native_scanout_release"));

    const auto reuse = Block(source, "if (slot.read.lease)");
    EXPECT_TRUE(reuse.has("slot.read.allocation.content_generation != snapshot.allocation.content_generation"));
    EXPECT_TRUE(reuse.has("switch_native_scanout_release(&snapshot); return -1;"));
    EXPECT_FALSE(reuse.has("releaseRead"));
    EXPECT_FALSE(reuse.has("slot.read ="));

    const auto lifecycle = SourceText("../SwitchExternalScreen.cpp");
    const auto off = Block(lifecycle, "void SwitchExternalScreen::linkDown()");
    EXPECT_TRUE(off.has("releaseRead(static_cast<unsigned>(front));"));
    const auto detach = Block(lifecycle, "void SwitchExternalScreen::detach()");
    EXPECT_TRUE(detach.has("releaseSlot(i);"));
    EXPECT_TRUE(detach.has("drainSwapQueue();"));
    EXPECT_TRUE(detach.has("publishActiveGeometry();"));
    EXPECT_TRUE(Block(lifecycle, "void SwitchExternalScreen::modeFailed()").has("drainSwapQueue();"));
    const auto select = Block(lifecycle, "IOReturn SwitchExternalScreen::selectOutput(");
    EXPECT_TRUE(select.has("self->publishActiveGeometry();"));
    EXPECT_TRUE(select.has("if (device != 2) self->drainSwapQueue();"));
    EXPECT_TRUE(Block(lifecycle, "void SwitchExternalScreen::linkUp(").has("mode.hactive != timing.hactive"));

    const auto validate = Block(lifecycle, "bool SwitchExternalScreen::validateSwap(");
    EXPECT_TRUE(validate.has("__atomic_load_n(&activeGeometry, __ATOMIC_ACQUIRE)"));
    EXPECT_FALSE(validate.has("modes.modes[selectedMode]"));

    const auto prepare = Block(source, "int SwitchExternalScreen::prepareSlot(");
    EXPECT_TRUE(prepare.has("native_legacy_surface_bounds("));
    EXPECT_TRUE(prepare.has("if (!slot.layout && !applyTiledLayout("));

    const auto request = Block(source, "void SwitchExternalScreen::requestFrame()");
    EXPECT_TRUE(request.has("offer.width = slots[index].width;"));
    EXPECT_TRUE(request.has("offer.height = slots[index].height;"));
    EXPECT_FALSE(request.has("offer.width = mode.hactive;"));
    EXPECT_FALSE(request.has("offer.height = mode.vactive;"));
    EXPECT_TRUE(request.has("TEGRA_SCANOUT_COPY_SOURCE_BLOCK"));
    EXPECT_TRUE(request.has("offer.sourceBlockHeightLog2 = tiled ? ((slots[index].kind >> 4) & 7u) : 0u;"));

    const auto complete = Block(source, "void SwitchExternalScreen::completionFired(");
    EXPECT_TRUE(complete.has("if (!self->connected || self->selectedMode < 0 || self->device != 2)"));
    EXPECT_TRUE(complete.has("self->drainSwapQueue();"));

    const auto limits = Block(lifecycle, "void SwitchExternalScreen::publishScanoutLimits()");
    EXPECT_TRUE(limits.has("IOMFBYUV420UnSupported"));
    EXPECT_TRUE(limits.has("IOMFBDisableFlip"));
    EXPECT_TRUE(limits.has("publishSourceLimits(148500000u)"));
    EXPECT_NE(lifecycle.find("SwitchExternalScreen::getMatrix("), std::string::npos);
    EXPECT_NE(lifecycle.find("SwitchExternalScreen::setMatrix("), std::string::npos);
    EXPECT_NE(lifecycle.find("SwitchExternalScreen::swapSetColorMatrix("), std::string::npos);
    EXPECT_NE(lifecycle.find("SwitchExternalScreen::setAccessibilityScale("), std::string::npos);

    const auto header = SourceText("../SwitchExternalScreen.h");
    EXPECT_NE(header.find("kSlotSpan = 16ULL << 20"), std::string::npos);
    EXPECT_NE(header.find("static_assert(kTablePages == 13);"), std::string::npos);
    EXPECT_EQ(header.find("getWorkLoop()"), std::string::npos);
    EXPECT_NE(header.find("setPowerState(unsigned long ordinal, IOService *whatDevice) override"), std::string::npos);
    EXPECT_NE(header.find("void stop(IOService *provider) override"), std::string::npos);

    const auto service = SourceText("../SwitchDisplayPort.cpp");
    const auto beginPort = Block(service, "bool SwitchDisplayPort::begin()");
    EXPECT_TRUE(beginPort.before("screen = OSTypeAlloc(SwitchExternalScreen);", "loop = screen->getWorkLoop();"));
    EXPECT_TRUE(beginPort.before("loop = screen->getWorkLoop();", "loop->retain();"));
    EXPECT_TRUE(beginPort.before("loop->retain();", "screen->begin(this, loop)"));
    EXPECT_FALSE(beginPort.has("IOWorkLoop::workLoop()"));

    const auto beginScreen = Block(lifecycle, "bool SwitchExternalScreen::begin(");
    EXPECT_TRUE(beginScreen.before("setInternalDisplay(false);", "IOMobileFramebufferLegacy::start(nub)"));
    EXPECT_FALSE(beginScreen.has("registerService();"));
    EXPECT_TRUE(beginScreen.before("PMinit();", "nub->joinPMtree(this);"));
    EXPECT_TRUE(beginScreen.before("nub->joinPMtree(this);", "registerPowerDriver(this, states, 2)"));
    EXPECT_TRUE(beginScreen.before("registerPowerDriver(this, states, 2)", "changePowerStateToPriv(1)"));

    const auto power = Block(lifecycle, "IOReturn SwitchExternalScreen::setPowerState(");
    EXPECT_TRUE(power.has("if (ordinal > 1) return kIOReturnUnsupported;"));
    EXPECT_TRUE(power.has("IOMobileFramebufferLegacy::setPowerState(ordinal, whatDevice)"));
    EXPECT_TRUE(power.has("self->drainSwapQueue();"));
    EXPECT_TRUE(power.has("self->kickSwapQueue();"));

    const auto publish = Block(lifecycle, "bool SwitchExternalScreen::publish(");
    EXPECT_TRUE(publish.before("setProperty(\"IOMFBHotplugKeysChangedKey\", kOSBooleanTrue);",
                               "IOMobileFramebufferLegacy::hotPlugNotify(1);"));
    const auto clearProps = Block(lifecycle, "void SwitchExternalScreen::clearSinkProperties()");
    EXPECT_TRUE(clearProps.has("removeProperty(\"IOMFBHotplugKeysChangedKey\");"));
    const auto publishGeom = Block(lifecycle, "void SwitchExternalScreen::publishActiveGeometry()");
    EXPECT_TRUE(publishGeom.has("connected && device == 2 && selectedMode >= 0"));

    EXPECT_TRUE(detach.has("setDisplayWidth(0);"));
    EXPECT_TRUE(detach.has("setDisplayHeight(0);"));
    EXPECT_TRUE(Block(lifecycle, "void SwitchExternalScreen::linkUp(").has("dcb->frameInterrupt(true);"));
    EXPECT_TRUE(select.before("self->device = device;", "self->transport->selectMode(index, device == 2)"));
    EXPECT_TRUE(validate.has("layer.transform = transforms[i] & 7u;"));

    const auto mapDom = Block(source, "bool SwitchExternalScreen::mapDomain()");
    EXPECT_TRUE(mapDom.has("(1u << 2) | (1u << 4) | (1u << 6)"));
    EXPECT_FALSE(mapDom.has("1u << 17"));
    EXPECT_TRUE(prepare.has("allocation.pages[offset / 4096]"));
    EXPECT_FALSE(prepare.has("allocation.pages[0]"));

    EXPECT_TRUE(latch.has("if (receipt == TegraDisplayB::Latch::Missed)"));
    EXPECT_TRUE(latch.has("dcb->resetWindowCache();"));
    EXPECT_FALSE(latch.has("return false;"));
    EXPECT_TRUE(latch.has("dcb->frameInterrupt(true);"));

    const auto dcIrq = Block(service, "void SwitchDisplayPort::dcInterrupt(");
    EXPECT_FALSE(dcIrq.has("self->fail(\"DCB\""));

    const auto selectMode = Block(service, "IOReturn SwitchDisplayPort::selectMode(");
    EXPECT_TRUE(selectMode.has("gate->runAction("));
    EXPECT_TRUE(selectMode.has("}, &req);"));
    EXPECT_FALSE(selectMode.has("this, &req"));
    EXPECT_TRUE(selectMode.has("commandSleep(&self->trainingEvent, deadline, THREAD_UNINT)"));
    EXPECT_TRUE(selectMode.has("self->stage == Stage::Running && self->wantedMode == r.index"));

    const auto session = SourceText("../SwitchDPSession.cpp");
    EXPECT_TRUE(Block(session, "void SwitchDisplayPort::trained()").has("wakeModeWaiters(false);"));

    const auto teardown = Block(service, "void SwitchDisplayPort::teardownLink()");
    EXPECT_TRUE(teardown.before("dcb.powerDown();", "sor.powerDown();"));
    const auto auxTimer = Block(service, "void SwitchDisplayPort::auxTimerFired(");
    EXPECT_FALSE(auxTimer.has("panic("));
    EXPECT_TRUE(auxTimer.has("self->aux.resetAux();"));
    EXPECT_TRUE(auxTimer.has("self->cecComplete(false);"));
    EXPECT_TRUE(auxTimer.has("self->fail(\"DPAUX1\", why);"));

    EXPECT_TRUE(publish.before("tegra_gm20b_set_external_scanout(1);", "IOMobileFramebufferLegacy::hotPlugNotify(1);"));
    EXPECT_TRUE(detach.has("tegra_gm20b_set_external_scanout(0);"));
    EXPECT_TRUE(validate.has("tegra_gm20b_surface_compressed(firstPa & ~0xfffULL)"));
    const auto tiledLayout = Block(source, "bool SwitchExternalScreen::applyTiledLayout(");
    EXPECT_TRUE(tiledLayout.before("tegra_gm20b_surface_hold(firstPage, &tiled)", "__atomic_thread_fence(__ATOMIC_SEQ_CST);"));
    EXPECT_TRUE(tiledLayout.before("__atomic_thread_fence(__ATOMIC_SEQ_CST);", "tegra_gm20b_surface_compressed(firstPage)"));
    EXPECT_TRUE(prepare.before("__atomic_thread_fence(__ATOMIC_SEQ_CST);", "tegra_gm20b_surface_compressed(firstPage)"));
    EXPECT_TRUE(prepare.has("if (heldError == -16) return -16;"));
    EXPECT_TRUE(request.has("slots[index].read.view.waits[i]"));
    EXPECT_TRUE(request.has("switch_host1x_token_fence("));
    EXPECT_FALSE(request.has("switch_native_scanout_dependencies("));
    EXPECT_TRUE(publish.before("native_legacy_set_display_refresh_properties(this)",
                               "IOMobileFramebufferLegacy::hotPlugNotify(1);"));
    EXPECT_TRUE(Block(lifecycle, "bool SwitchExternalScreen::getRefreshAttributes(").has("if (!refreshPeriod)"));
}
