// Copyright (c) Facebook, Inc. and its affiliates.
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <Corrade/Containers/Optional.h>
#include <Corrade/Containers/StridedArrayView.h>
#include <Corrade/Utility/Arguments.h>
#include <Corrade/Utility/Format.h>
#include <Magnum/GL/DefaultFramebuffer.h>
#include <Magnum/DebugTools/FrameProfiler.h>
#include <Magnum/Math/ConfigurationValue.h>
#include <Magnum/Platform/GlfwApplication.h>

#include "MagnumRenderer.h"

namespace esp { namespace batched_sim {

namespace Cr = Corrade;
namespace Mn = Magnum;

using namespace Cr::Containers::Literals;
using namespace Mn::Math::Literals;

class MagnumRendererDemo: public Mn::Platform::Application {
  public:
    explicit MagnumRendererDemo(const Arguments& arguments);

  private:
    void drawEvent() override;

    Cr::Containers::Optional<MagnumRenderer> _renderer;
    Mn::DebugTools::FrameProfilerGL _profiler;
};

MagnumRendererDemo::MagnumRendererDemo(const Arguments& arguments): Mn::Platform::Application{arguments, Mn::NoCreate} {
  Cr::Utility::Arguments args;
  args.addArgument("file").setHelp("file", "bps file to load")
      .addOption('I', "importer")
        .setHelp("importer", "importer plugin to use instead of BpsImporter")
      .addBooleanOption("profile")
        .setHelp("profile", "profile frame times")
      .addOption('S', "size", "128 128")
        .setHelp("size", "size of one rendered tile")
      .addOption('C', "count", "16 12")
        .setHelp("count", "tile count")
      .addBooleanOption("no-textures")
        .setHelp("no-textures", "render without textures")
      .addSkippedPrefix("magnum", "engine-specific options")
      .parse(arguments.argc, arguments.argv);

  const Mn::Vector2i tileSize = args.value<Mn::Vector2i>("size");
  const Mn::Vector2i tileCount = args.value<Mn::Vector2i>("count");

  /* Create a window with size matching the tile count & size */
  create(Configuration{}
    .setSize(tileSize*tileCount, Mn::Vector2{1.0f})
    .setTitle("Magnum Renderer Demo"));

  /* Create the renderer */
  MagnumRendererConfiguration configuration;
  configuration
    .setFlags(args.isSet("no-textures") ? MagnumRendererFlag::NoTextures : MagnumRendererFlags{})
    .setTileSizeCount(tileSize, tileCount);
  _renderer.emplace(configuration);

  if(!args.value("importer").empty())
    _renderer->addFile(args.value("file"), args.value("importer"));
  else  // TODO handle this fallback inside Renderer directly
    _renderer->addFile(args.value("file"));

  /* Hardcode camera position + projection for all views to be above the scene */
  for(std::size_t i = 0; i != _renderer->sceneCount(); ++i) {
    _renderer->camera(i) =
      Mn::Matrix4::perspectiveProjection(35.0_degf, Mn::Vector2{tileSize}.aspectRatio(), 0.01f, 1000.0f)*
      (Mn::Matrix4::rotationX(-90.0_degf)*Mn::Matrix4::translation(Mn::Vector3::zAxis(20.0f))).inverted();

    /* Add stuff */
    _renderer->add(i, Cr::Utility::format("Baked_sc{}_staging_{:.2}", i/21, i%21));
  }

  Mn::Debug{} << "Rendering" << tileCount.product() << tileSize << "tiles every frame";

  if(args.isSet("profile")) {
    _profiler = Mn::DebugTools::FrameProfilerGL{
      Mn::DebugTools::FrameProfilerGL::Value::CpuDuration|
      Mn::DebugTools::FrameProfilerGL::Value::GpuDuration, 50};
  } else _profiler.disable();
}

void MagnumRendererDemo::drawEvent() {
  for(std::size_t i = 0; i != _renderer->sceneCount(); ++i) {
    /* Rotate the first-ever object in the scene */
    Mn::Matrix4& transformation = _renderer->transformations(i)[0];
    transformation = transformation*Mn::Matrix4::rotationY(0.05_degf*Mn::Float(i));
  }

  Mn::GL::defaultFramebuffer.clear(Mn::GL::FramebufferClear::Color|Mn::GL::FramebufferClear::Depth);

  _profiler.beginFrame();

  _renderer->draw(Mn::GL::defaultFramebuffer);

  _profiler.endFrame();
  _profiler.printStatistics(10);

  swapBuffers();
  if(_profiler.isEnabled()) redraw();
}

}}

MAGNUM_APPLICATION_MAIN(esp::batched_sim::MagnumRendererDemo)
