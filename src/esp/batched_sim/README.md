# How?

Build as usual. There should appear a `MagnumRendererConverter` binary and a
`MagnumRendererDemo` binary somewhere in the build dir.

# Running the converter

Expects the same data as the BPS conversion pipeline, except maybe extra
subdirectories. Patch the source if it misbehaves.

```sh
./RelWithDebInfo/bin/MagnumRendererConverter path/to/input-data/ yay.gltf
```

Output is a `yay.gltf`, `yay.bin` and `yay.0.ktx2`, with 105 Replica scenes
and deduplicated meshes. Not Basis-compressed, because a 2048x2048x105 texture
needs lots of memory, that has to be done separately using for example the
`magnum-sceneconverter` utility (take it from the prebuilt tools):

```sh
magnum-imageconverter -D3 --map -v -C BasisImageConverter \
  -c uastc,rdo_uastc,rdo_uastc_dict_size=1024,ktx2_zstd_supercompression_level=20,mip_gen,threads=0,y_flip=false \
  yay.0.ktx2 yay.0.basis.ktx2
```

Takes about 25 minutes and 12 GB RAM on a 8-core laptop from 2018. Once done,
rename the output back to `yay.0.ktx2`. Now the input glTF is Basis-compressed.

# Running the demo

With the file you just generated:

```sh
./RelWithDebInfo/bin/MagnumRendererDemo \
  --count "15 7" --size "128 192" --profile \
  yay.gltf
```

It could work, hopefully? Profiling info goes into the console.

-------------------------------------------------------------------------------

# Outdated info below, DO NOT READ

# Runing the demo

Use `--profile` to profile frame times to console.

```sh
cd data/bps_data/combined_Stage_v3_sc0_staging
../../../build-externalmagnum/esp/batched_sim/BatchRendering combined_Stage_v3_sc0_staging_trimesh.bps -I ../../../build-externalmagnum/esp/batched_sim/BpsImporter.so
```

```sh
cd data/bps_data/combined_Stage_v3_sc0_staging
../../../build-externalmagnum/esp/batched_sim/MagnumRendererDemo combined_Stage_v3_sc0_staging_trimesh.bps -I ../../../build-externalmagnum/esp/batched_sim/BpsImporter.so
```

# Info about imported data

Use `--info` to display also info about images, textures and materials. Add
the following options to `-i` to show different variants:

-   `meshViews` to import one mesh with several views
-   `textureArrays` to import one 2D array texture with several layers

```sh
cd data/bps_data/combined_Stage_v3_sc0_staging
magnum-sceneconverter combined_Stage_v3_sc0_staging_trimesh.bps -I ../../../build-externalmagnum/esp/batched_sim/BpsImporter.so -i basisFormat=Astc4x4RGBA --map --info-scenes --info-meshes
```

# Opening the BPS data in magnum-player

Pick different format if on a NV GPU (e.g. `Bc7RGBA`): Add `meshViews` to `-i`
to import one mesh with several views (which `magnum-player` doesn't currently
understand).

```sh
cd data/bps_data/combined_Stage_v3_sc0_staging
magnum-player combined_Stage_v3_sc0_staging_trimesh.bps -I ../../../build-externalmagnum/esp/batched_sim/BpsImporter.so -i basisFormat=Astc4x4RGBA --map
```

# Converting to a glTF

```sh
magnum-sceneconverter -i basisFormat=Astc4x4RGBA,textureArrays,meshViews,instanceScene=false,textureArraysForceAllMaterialsTextured -I ~/Code/fair/batch/gala_kinematic/build-externalmagnum/esp/batched_sim/BpsImporter.so -C ~/Code/magnum-plugins/build/Debug/lib/magnum-d/sceneconverters/GltfSceneConverter.so ~/Code/fair/batch/gala_kinematic/data/bps_data/replicacad_composite/replicacad_composite.bps replicacad_composite.views.latest.gltf -c extensionRequired=MAGNUMX_mesh_views,extensionUsed=MAGNUMX_mesh_views
```

info:

```sh
magnum-sceneconverter -i basisFormat=Astc4x4RGBA,textureArrays,meshViews,instanceScene=false,phongMaterialFallback=false,meshlets=false -I ~/Code/fair/batch/gala_kinematic/build-externalmagnum/esp/batched_sim/BpsImporter.so ~/Code/fair/batch/gala_kinematic/data/bps_data/replicacad_composite/replicacad_composite.bps replicacad_composite.views.latest.gltf --info -i phongMaterialFallback=false
```

info on the produced gltf

```sh
magnum-sceneconverter -i phongMaterialFallback=false,ignoreRequiredExtensions,experimentalKhrTextureKtx -I ~/Code/magnum-plugins/build/Debug/lib/magnum-d/importers/GltfImporter.so replicacad_composite.views.latest.gltf --info -i phongMaterialFallback=false,ignoreRequiredExtensions,experimentalKhrTextureKtx
```

# Producing a glTF from scratch

```sh
cd path/where/all/data/is
~/Code/fair/batch/gala_kinematic/build-externalmagnum/esp/batched_sim/MagnumRendererConverter . yay.gltf -C ~/Code/magnum-plugins/build/Debug/lib/magnum-d/sceneconverters/GltfSceneConverter.so
```
