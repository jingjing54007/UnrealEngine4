// Copyright 1998-2015 Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	GlobalDistanceField.cpp
=============================================================================*/

#include "RendererPrivate.h"
#include "ScenePrivate.h"
#include "UniformBuffer.h"
#include "ShaderParameters.h"
#include "PostProcessing.h"
#include "SceneFilterRendering.h"
#include "DistanceFieldLightingShared.h"
#include "DistanceFieldSurfaceCacheLighting.h"
#include "GlobalDistanceField.h"
#include "RHICommandList.h"
#include "SceneUtils.h"

int32 GAOGlobalDistanceField = 1;
FAutoConsoleVariableRef CVarAOGlobalDistanceField(
	TEXT("r.AOGlobalDistanceField"),
	GAOGlobalDistanceField,
	TEXT("Whether to use a global distance field to optimize occlusion cone traces.\n")
	TEXT("The global distance field is created by compositing object distance fields into clipmaps as the viewer moves through the level."),
	ECVF_Cheat | ECVF_RenderThreadSafe
	);

int32 GAOUpdateGlobalDistanceField = 1;
FAutoConsoleVariableRef CVarAOUpdateGlobalDistanceField(
	TEXT("r.AOUpdateGlobalDistanceField"),
	GAOUpdateGlobalDistanceField,
	TEXT("Whether to update the global distance field, useful for debugging."),
	ECVF_Cheat | ECVF_RenderThreadSafe
	);

int32 GAOGlobalDistanceFieldPartialUpdates = 1;
FAutoConsoleVariableRef CVarAOGlobalDistanceFieldPartialUpdates(
	TEXT("r.AOGlobalDistanceFieldPartialUpdates"),
	GAOGlobalDistanceFieldPartialUpdates,
	TEXT("Whether to allow partial updates of the global distance field.  When profiling it's useful to disable this and get the worst case composition time that happens on camera cuts."),
	ECVF_Cheat | ECVF_RenderThreadSafe
	);

int32 GAOVisualizeGlobalDistanceField = 0;
FAutoConsoleVariableRef CVarAOVisualizeGlobalDistanceField(
	TEXT("r.AOVisualizeGlobalDistanceField"),
	GAOVisualizeGlobalDistanceField,
	TEXT("Whether to visualize the global distance field instead of the object DFs with the 'Mesh Distance Fields' Visualize mode"),
	ECVF_Cheat | ECVF_RenderThreadSafe
	);

float GAOInnerGlobalDFClipmapDistance = 2500;
FAutoConsoleVariableRef CVarAOInnerGlobalDFClipmapDistance(
	TEXT("r.AOInnerGlobalDFClipmapDistance"),
	GAOInnerGlobalDFClipmapDistance,
	TEXT("World space distance from the camera that the first clipmap contains."),
	ECVF_Cheat | ECVF_RenderThreadSafe
	);

float GAOGlobalDFClipmapDistanceExponent = 2;
FAutoConsoleVariableRef CVarAOGlobalDFClipmapDistanceExponent(
	TEXT("r.AOGlobalDFClipmapDistanceExponent"),
	GAOGlobalDFClipmapDistanceExponent,
	TEXT("Exponent used to derive each clipmap's size, together with r.AOInnerGlobalDFClipmapDistance."),
	ECVF_Cheat | ECVF_RenderThreadSafe
	);

int32 GAOGlobalDFResolution = 128;
FAutoConsoleVariableRef CVarAOGlobalDFResolution(
	TEXT("r.AOGlobalDFResolution"),
	GAOGlobalDFResolution,
	TEXT("Resolution of the global distance field.  Higher values increase fidelity but also increase memory and composition cost."),
	ECVF_Cheat | ECVF_RenderThreadSafe
	);

float GAOGlobalDFStartDistance = 200;
FAutoConsoleVariableRef CVarAOGlobalDFStartDistance(
	TEXT("r.AOGlobalDFStartDistance"),
	GAOGlobalDFStartDistance,
	TEXT("World space distance along a cone trace to switch to using the global distance field instead of the object distance fields.\n")
	TEXT("This has to be large enough to hide the low res nature of the global distance field, but smaller values result in faster cone tracing."),
	ECVF_Cheat | ECVF_RenderThreadSafe
	);

TGlobalResource<FDistanceFieldObjectBufferResource> GGlobalDistanceFieldCulledObjectBuffers;

uint32 CullObjectsGroupSize = 64;

class FCullObjectsForVolumeCS : public FGlobalShader
{
	DECLARE_SHADER_TYPE(FCullObjectsForVolumeCS,Global)
public:

	static bool ShouldCache(EShaderPlatform Platform)
	{
		return IsFeatureLevelSupported(Platform, ERHIFeatureLevel::SM5) && DoesPlatformSupportDistanceFieldAO(Platform);
	}

	static void ModifyCompilationEnvironment(EShaderPlatform Platform, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Platform,OutEnvironment);
		OutEnvironment.SetDefine(TEXT("CULLOBJECTS_THREADGROUP_SIZE"), CullObjectsGroupSize);
	}

	FCullObjectsForVolumeCS(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
		: FGlobalShader(Initializer)
	{
		ObjectBufferParameters.Bind(Initializer.ParameterMap);
		ObjectIndirectArguments.Bind(Initializer.ParameterMap, TEXT("ObjectIndirectArguments"));
		CulledObjectBounds.Bind(Initializer.ParameterMap, TEXT("CulledObjectBounds"));
		CulledObjectData.Bind(Initializer.ParameterMap, TEXT("CulledObjectData"));
		CulledObjectBoxBounds.Bind(Initializer.ParameterMap, TEXT("CulledObjectBoxBounds"));
		AOGlobalMaxSphereQueryRadius.Bind(Initializer.ParameterMap, TEXT("AOGlobalMaxSphereQueryRadius"));
		VolumeBounds.Bind(Initializer.ParameterMap, TEXT("VolumeBounds"));
	}

	FCullObjectsForVolumeCS()
	{
	}

	void SetParameters(FRHICommandList& RHICmdList, const FScene* Scene, const FSceneView& View, float MaxOcclusionDistance, const FVector4& VolumeBoundsValue)
	{
		FComputeShaderRHIParamRef ShaderRHI = GetComputeShader();
		FGlobalShader::SetParameters(RHICmdList, ShaderRHI, View);
		ObjectBufferParameters.Set(RHICmdList, ShaderRHI, *(Scene->DistanceFieldSceneData.ObjectBuffers), Scene->DistanceFieldSceneData.NumObjectsInBuffer);

		ObjectIndirectArguments.SetBuffer(RHICmdList, ShaderRHI, GGlobalDistanceFieldCulledObjectBuffers.Buffers.ObjectIndirectArguments);
		CulledObjectBounds.SetBuffer(RHICmdList, ShaderRHI, GGlobalDistanceFieldCulledObjectBuffers.Buffers.Bounds);
		CulledObjectData.SetBuffer(RHICmdList, ShaderRHI, GGlobalDistanceFieldCulledObjectBuffers.Buffers.Data);
		CulledObjectBoxBounds.SetBuffer(RHICmdList, ShaderRHI, GGlobalDistanceFieldCulledObjectBuffers.Buffers.BoxBounds);

		extern float GAOConeHalfAngle;
		const float GlobalMaxSphereQueryRadius = MaxOcclusionDistance / (1 + FMath::Tan(GAOConeHalfAngle));
		SetShaderValue(RHICmdList, ShaderRHI, AOGlobalMaxSphereQueryRadius, GlobalMaxSphereQueryRadius);
		SetShaderValue(RHICmdList, ShaderRHI, VolumeBounds, VolumeBoundsValue);
	}

	void UnsetParameters(FRHICommandList& RHICmdList)
	{
		ObjectBufferParameters.UnsetParameters(RHICmdList, GetComputeShader());
		ObjectIndirectArguments.UnsetUAV(RHICmdList, GetComputeShader());
		CulledObjectBounds.UnsetUAV(RHICmdList, GetComputeShader());
		CulledObjectData.UnsetUAV(RHICmdList, GetComputeShader());
		CulledObjectBoxBounds.UnsetUAV(RHICmdList, GetComputeShader());
	}

	virtual bool Serialize(FArchive& Ar)
	{		
		bool bShaderHasOutdatedParameters = FGlobalShader::Serialize(Ar);
		Ar << ObjectBufferParameters;
		Ar << ObjectIndirectArguments;
		Ar << CulledObjectBounds;
		Ar << CulledObjectData;
		Ar << CulledObjectBoxBounds;
		Ar << AOGlobalMaxSphereQueryRadius;
		Ar << VolumeBounds;
		return bShaderHasOutdatedParameters;
	}

private:

	FDistanceFieldObjectBufferParameters ObjectBufferParameters;
	FRWShaderParameter ObjectIndirectArguments;
	FRWShaderParameter CulledObjectBounds;
	FRWShaderParameter CulledObjectData;
	FRWShaderParameter CulledObjectBoxBounds;
	FShaderParameter AOGlobalMaxSphereQueryRadius;
	FShaderParameter VolumeBounds;
};

IMPLEMENT_SHADER_TYPE(,FCullObjectsForVolumeCS,TEXT("GlobalDistanceField"),TEXT("CullObjectsForVolumeCS"),SF_Compute);

const int32 GMaxGridCulledObjects = 2047;

class FObjectGridBuffers : public FRenderResource
{
public:

	int32 GridDimension;
	FRWBuffer CulledObjectGrid;

	FObjectGridBuffers()
	{
		GridDimension = 0;
	}

	virtual void InitDynamicRHI()  override
	{
		if (GridDimension > 0)
		{
			CulledObjectGrid.Initialize(sizeof(uint32), GridDimension * GridDimension * GridDimension * (GMaxGridCulledObjects + 1), PF_R32_UINT);
		}
	}

	virtual void ReleaseDynamicRHI() override
	{
		CulledObjectGrid.Release();
	}

	size_t GetSizeBytes() const
	{
		return CulledObjectGrid.NumBytes;
	}
};

TGlobalResource<FObjectGridBuffers> GObjectGridBuffers;

const int32 GCullGridTileSize = 16;

class FCullObjectsToGridCS : public FGlobalShader
{
	DECLARE_SHADER_TYPE(FCullObjectsToGridCS,Global)
public:

	static bool ShouldCache(EShaderPlatform Platform)
	{
		return IsFeatureLevelSupported(Platform, ERHIFeatureLevel::SM5) && DoesPlatformSupportDistanceFieldAO(Platform);
	}

	static void ModifyCompilationEnvironment(EShaderPlatform Platform, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Platform,OutEnvironment);
		OutEnvironment.SetDefine(TEXT("CULL_GRID_TILE_SIZE"), GCullGridTileSize);
		OutEnvironment.SetDefine(TEXT("MAX_GRID_CULLED_DF_OBJECTS"), GMaxGridCulledObjects);
	}

	FCullObjectsToGridCS(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
		: FGlobalShader(Initializer)
	{
		CulledObjectBufferParameters.Bind(Initializer.ParameterMap);
		GlobalDistanceFieldParameters.Bind(Initializer.ParameterMap);
		CulledObjectGrid.Bind(Initializer.ParameterMap, TEXT("CulledObjectGrid"));
		CullGridDimension.Bind(Initializer.ParameterMap, TEXT("CullGridDimension"));
		VolumeTexelSize.Bind(Initializer.ParameterMap, TEXT("VolumeTexelSize"));
		UpdateRegionVolumeMin.Bind(Initializer.ParameterMap, TEXT("UpdateRegionVolumeMin"));
		ClipmapIndex.Bind(Initializer.ParameterMap, TEXT("ClipmapIndex"));
		AOGlobalMaxSphereQueryRadius.Bind(Initializer.ParameterMap, TEXT("AOGlobalMaxSphereQueryRadius"));
	}

	FCullObjectsToGridCS()
	{
	}

	void SetParameters(
		FRHICommandList& RHICmdList, 
		const FScene* Scene, 
		const FSceneView& View, 
		float MaxOcclusionDistance, 
		const FGlobalDistanceFieldInfo& GlobalDistanceFieldInfo,
		int32 ClipmapIndexValue,
		const FVolumeUpdateRegion& UpdateRegion)
	{
		FComputeShaderRHIParamRef ShaderRHI = GetComputeShader();
		FGlobalShader::SetParameters(RHICmdList, ShaderRHI, View);
		CulledObjectBufferParameters.Set(RHICmdList, ShaderRHI, GGlobalDistanceFieldCulledObjectBuffers.Buffers);
		GlobalDistanceFieldParameters.Set(RHICmdList, ShaderRHI, GlobalDistanceFieldInfo);

		CulledObjectGrid.SetBuffer(RHICmdList, ShaderRHI, GObjectGridBuffers.CulledObjectGrid);

		const FIntVector GridDimensionValue(
			FMath::DivideAndRoundUp<int32>(UpdateRegion.CellsSize.X, GCullGridTileSize),
			FMath::DivideAndRoundUp<int32>(UpdateRegion.CellsSize.Y, GCullGridTileSize),
			FMath::DivideAndRoundUp<int32>(UpdateRegion.CellsSize.Z, GCullGridTileSize));

		SetShaderValue(RHICmdList, ShaderRHI, CullGridDimension, GridDimensionValue);
		SetShaderValue(RHICmdList, ShaderRHI, VolumeTexelSize, FVector(1.0f / GAOGlobalDFResolution));
		SetShaderValue(RHICmdList, ShaderRHI, UpdateRegionVolumeMin, UpdateRegion.Bounds.Min);
		SetShaderValue(RHICmdList, ShaderRHI, ClipmapIndex, ClipmapIndexValue);

		extern float GAOConeHalfAngle;
		const float GlobalMaxSphereQueryRadius = MaxOcclusionDistance / (1 + FMath::Tan(GAOConeHalfAngle));
		SetShaderValue(RHICmdList, ShaderRHI, AOGlobalMaxSphereQueryRadius, GlobalMaxSphereQueryRadius);
	}

	void UnsetParameters(FRHICommandList& RHICmdList)
	{
		CulledObjectGrid.UnsetUAV(RHICmdList, GetComputeShader());
	}

	virtual bool Serialize(FArchive& Ar)
	{		
		bool bShaderHasOutdatedParameters = FGlobalShader::Serialize(Ar);
		Ar << CulledObjectBufferParameters;
		Ar << GlobalDistanceFieldParameters;
		Ar << CulledObjectGrid;
		Ar << CullGridDimension;
		Ar << VolumeTexelSize;
		Ar << UpdateRegionVolumeMin;
		Ar << ClipmapIndex;
		Ar << AOGlobalMaxSphereQueryRadius;
		return bShaderHasOutdatedParameters;
	}

private:

	FDistanceFieldCulledObjectBufferParameters CulledObjectBufferParameters;
	FGlobalDistanceFieldParameters GlobalDistanceFieldParameters;
	FRWShaderParameter CulledObjectGrid;
	FShaderParameter CullGridDimension;
	FShaderParameter VolumeTexelSize;
	FShaderParameter UpdateRegionVolumeMin;
	FShaderParameter ClipmapIndex;
	FShaderParameter AOGlobalMaxSphereQueryRadius;
};

IMPLEMENT_SHADER_TYPE(,FCullObjectsToGridCS,TEXT("GlobalDistanceField"),TEXT("CullObjectsToGridCS"),SF_Compute);

const int32 CompositeTileSize = 4;

class FCompositeObjectDistanceFieldsCS : public FGlobalShader
{
	DECLARE_SHADER_TYPE(FCompositeObjectDistanceFieldsCS,Global)
public:

	static bool ShouldCache(EShaderPlatform Platform)
	{
		return IsFeatureLevelSupported(Platform, ERHIFeatureLevel::SM5) && DoesPlatformSupportDistanceFieldAO(Platform);
	}

	static void ModifyCompilationEnvironment(EShaderPlatform Platform, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Platform,OutEnvironment);
		OutEnvironment.SetDefine(TEXT("COMPOSITE_THREADGROUP_SIZE"), CompositeTileSize);
		OutEnvironment.SetDefine(TEXT("CULL_GRID_TILE_SIZE"), GCullGridTileSize);
		OutEnvironment.SetDefine(TEXT("MAX_GRID_CULLED_DF_OBJECTS"), GMaxGridCulledObjects);
	}

	FCompositeObjectDistanceFieldsCS(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
		: FGlobalShader(Initializer)
	{
		CulledObjectBufferParameters.Bind(Initializer.ParameterMap);
		GlobalDistanceFieldParameters.Bind(Initializer.ParameterMap);
		GlobalDistanceFieldTexture.Bind(Initializer.ParameterMap, TEXT("GlobalDistanceFieldTexture"));
		CulledObjectGrid.Bind(Initializer.ParameterMap, TEXT("CulledObjectGrid"));
		UpdateRegionSize.Bind(Initializer.ParameterMap, TEXT("UpdateRegionSize"));
		CullGridDimension.Bind(Initializer.ParameterMap, TEXT("CullGridDimension"));
		VolumeTexelSize.Bind(Initializer.ParameterMap, TEXT("VolumeTexelSize"));
		UpdateRegionVolumeMin.Bind(Initializer.ParameterMap, TEXT("UpdateRegionVolumeMin"));
		ClipmapIndex.Bind(Initializer.ParameterMap, TEXT("ClipmapIndex"));
		AOGlobalMaxSphereQueryRadius.Bind(Initializer.ParameterMap, TEXT("AOGlobalMaxSphereQueryRadius"));
	}

	FCompositeObjectDistanceFieldsCS()
	{
	}

	void SetParameters(
		FRHICommandList& RHICmdList, 
		const FScene* Scene, 
		const FSceneView& View, 
		float MaxOcclusionDistance, 
		const FGlobalDistanceFieldInfo& GlobalDistanceFieldInfo,
		int32 ClipmapIndexValue,
		const FVolumeUpdateRegion& UpdateRegion)
	{
		FComputeShaderRHIParamRef ShaderRHI = GetComputeShader();
		FGlobalShader::SetParameters(RHICmdList, ShaderRHI, View);
		CulledObjectBufferParameters.Set(RHICmdList, ShaderRHI, GGlobalDistanceFieldCulledObjectBuffers.Buffers);
		GlobalDistanceFieldParameters.Set(RHICmdList, ShaderRHI, GlobalDistanceFieldInfo);

		GlobalDistanceFieldTexture.SetTexture(RHICmdList, ShaderRHI, GlobalDistanceFieldInfo.Clipmaps[ClipmapIndexValue].RenderTarget->GetRenderTargetItem().ShaderResourceTexture, GlobalDistanceFieldInfo.Clipmaps[ClipmapIndexValue].RenderTarget->GetRenderTargetItem().UAV);

		SetSRVParameter(RHICmdList, ShaderRHI, CulledObjectGrid, GObjectGridBuffers.CulledObjectGrid.SRV);

		const FIntVector GridDimensionValue(
			FMath::DivideAndRoundUp<int32>(UpdateRegion.CellsSize.X, GCullGridTileSize),
			FMath::DivideAndRoundUp<int32>(UpdateRegion.CellsSize.Y, GCullGridTileSize),
			FMath::DivideAndRoundUp<int32>(UpdateRegion.CellsSize.Z, GCullGridTileSize));

		SetShaderValue(RHICmdList, ShaderRHI, CullGridDimension, GridDimensionValue);

		SetShaderValue(RHICmdList, ShaderRHI, UpdateRegionSize, UpdateRegion.CellsSize);
		SetShaderValue(RHICmdList, ShaderRHI, VolumeTexelSize, FVector(1.0f / GAOGlobalDFResolution));
		SetShaderValue(RHICmdList, ShaderRHI, UpdateRegionVolumeMin, UpdateRegion.Bounds.Min);
		SetShaderValue(RHICmdList, ShaderRHI, ClipmapIndex, ClipmapIndexValue);

		extern float GAOConeHalfAngle;
		const float GlobalMaxSphereQueryRadius = MaxOcclusionDistance / (1 + FMath::Tan(GAOConeHalfAngle));
		SetShaderValue(RHICmdList, ShaderRHI, AOGlobalMaxSphereQueryRadius, GlobalMaxSphereQueryRadius);
	}

	void UnsetParameters(FRHICommandList& RHICmdList)
	{
		GlobalDistanceFieldTexture.UnsetUAV(RHICmdList, GetComputeShader());
	}

	virtual bool Serialize(FArchive& Ar)
	{		
		bool bShaderHasOutdatedParameters = FGlobalShader::Serialize(Ar);
		Ar << CulledObjectBufferParameters;
		Ar << GlobalDistanceFieldParameters;
		Ar << GlobalDistanceFieldTexture;
		Ar << CulledObjectGrid;
		Ar << UpdateRegionSize;
		Ar << CullGridDimension;
		Ar << VolumeTexelSize;
		Ar << UpdateRegionVolumeMin;
		Ar << ClipmapIndex;
		Ar << AOGlobalMaxSphereQueryRadius;
		return bShaderHasOutdatedParameters;
	}

private:

	FDistanceFieldCulledObjectBufferParameters CulledObjectBufferParameters;
	FGlobalDistanceFieldParameters GlobalDistanceFieldParameters;
	FRWShaderParameter GlobalDistanceFieldTexture;
	FShaderResourceParameter CulledObjectGrid;
	FShaderParameter UpdateRegionSize;
	FShaderParameter CullGridDimension;
	FShaderParameter VolumeTexelSize;
	FShaderParameter UpdateRegionVolumeMin;
	FShaderParameter ClipmapIndex;
	FShaderParameter AOGlobalMaxSphereQueryRadius;
};

IMPLEMENT_SHADER_TYPE(,FCompositeObjectDistanceFieldsCS,TEXT("GlobalDistanceField"),TEXT("CompositeObjectDistanceFieldsCS"),SF_Compute);

/** Constructs and adds an update region based on camera movement for the given axis. */
void AddUpdateRegionForAxis(FIntVector Movement, const FBox& ClipmapBounds, float CellSize, int32 ComponentIndex, TArray<FVolumeUpdateRegion, TInlineAllocator<3> >& UpdateRegions)
{
	FVolumeUpdateRegion UpdateRegion;
	UpdateRegion.Bounds = ClipmapBounds;
	UpdateRegion.CellsSize = FIntVector(GAOGlobalDFResolution);
	UpdateRegion.CellsSize[ComponentIndex] = FMath::Min(FMath::Abs(Movement[ComponentIndex]), GAOGlobalDFResolution);

	if (Movement[ComponentIndex] > 0)
	{
		// Positive axis movement, set the min of that axis to contain the newly exposed area
		UpdateRegion.Bounds.Min[ComponentIndex] = FMath::Max(ClipmapBounds.Max[ComponentIndex] - Movement[ComponentIndex] * CellSize, ClipmapBounds.Min[ComponentIndex]);
	}
	else if (Movement[ComponentIndex] < 0)
	{
		// Negative axis movement, set the max of that axis to contain the newly exposed area
		UpdateRegion.Bounds.Max[ComponentIndex] = FMath::Min(ClipmapBounds.Min[ComponentIndex] - Movement[ComponentIndex] * CellSize, ClipmapBounds.Max[ComponentIndex]);
	}

	if (UpdateRegion.CellsSize[ComponentIndex] > 0)
	{
		UpdateRegions.Add(UpdateRegion);
	}
}

/** Constructs and adds an update region based on the given primitive bounds. */
void AddUpdateRegionForPrimitive(const FVector4& Bounds, const FBox& ClipmapBounds, float CellSize, TArray<FVolumeUpdateRegion, TInlineAllocator<3> >& UpdateRegions)
{
	FBox BoundingBox((FVector)Bounds - Bounds.W, (FVector)Bounds + Bounds.W);

	FVolumeUpdateRegion UpdateRegion;
	UpdateRegion.Bounds.Init();
	// Snap the min and clamp to clipmap bounds
	UpdateRegion.Bounds.Min.X = FMath::Max(CellSize * FMath::FloorToFloat(BoundingBox.Min.X / CellSize), ClipmapBounds.Min.X);
	UpdateRegion.Bounds.Min.Y = FMath::Max(CellSize * FMath::FloorToFloat(BoundingBox.Min.Y / CellSize), ClipmapBounds.Min.Y);
	UpdateRegion.Bounds.Min.Z = FMath::Max(CellSize * FMath::FloorToFloat(BoundingBox.Min.Z / CellSize), ClipmapBounds.Min.Z);

	// Derive the max from the snapped min and size, clamp to clipmap bounds
	UpdateRegion.Bounds.Max = UpdateRegion.Bounds.Min + FVector(FMath::CeilToFloat(Bounds.W * 2 / CellSize)) * CellSize;
	UpdateRegion.Bounds.Max.X = FMath::Min(UpdateRegion.Bounds.Max.X, ClipmapBounds.Max.X);
	UpdateRegion.Bounds.Max.Y = FMath::Min(UpdateRegion.Bounds.Max.Y, ClipmapBounds.Max.Y);
	UpdateRegion.Bounds.Max.Z = FMath::Min(UpdateRegion.Bounds.Max.Z, ClipmapBounds.Max.Z);

	const FVector UpdateRegionSize = UpdateRegion.Bounds.GetSize();
	UpdateRegion.CellsSize.X = FMath::TruncToInt(UpdateRegionSize.X / CellSize + .5f);
	UpdateRegion.CellsSize.Y = FMath::TruncToInt(UpdateRegionSize.Y / CellSize + .5f);
	UpdateRegion.CellsSize.Z = FMath::TruncToInt(UpdateRegionSize.Z / CellSize + .5f);

	// Only add update regions with positive area
	if (UpdateRegion.CellsSize.X > 0 && UpdateRegion.CellsSize.Y > 0 && UpdateRegion.CellsSize.Z > 0)
	{
		checkSlow(UpdateRegion.CellsSize.X <= GAOGlobalDFResolution && UpdateRegion.CellsSize.Y <= GAOGlobalDFResolution && UpdateRegion.CellsSize.Z <= GAOGlobalDFResolution);
		UpdateRegions.Add(UpdateRegion);
	}
}

void AllocateClipmapTexture(int32 ClipmapIndex, TRefCountPtr<IPooledRenderTarget>& Texture)
{
	const TCHAR* TextureName = TEXT("GlobalDistanceField0");

	if (ClipmapIndex == 1)
	{
		TextureName = TEXT("GlobalDistanceField1");
	}
	else if (ClipmapIndex == 2)
	{
		TextureName = TEXT("GlobalDistanceField2");
	}
	else if (ClipmapIndex == 3)
	{
		TextureName = TEXT("GlobalDistanceField3");
	}

	GRenderTargetPool.FindFreeElement(
		FPooledRenderTargetDesc(FPooledRenderTargetDesc::CreateVolumeDesc(
			GAOGlobalDFResolution,
			GAOGlobalDFResolution,
			GAOGlobalDFResolution,
			PF_R16F,
			0,
			TexCreate_ShaderResource | TexCreate_RenderTargetable | TexCreate_UAV,
			false)),
		Texture,
		TextureName 
	);
}

/** Staggers clipmap updates so there are only 2 per frame */
bool ShouldUpdateClipmapThisFrame(int32 ClipmapIndex, int32 GlobalDistanceFieldUpdateIndex)
{
	if (ClipmapIndex == 0)
	{
		return true;
	}
	else if (ClipmapIndex == 1)
	{
		return GlobalDistanceFieldUpdateIndex % 2 == 0;
	}
	else if (ClipmapIndex == 2)
	{
		return GlobalDistanceFieldUpdateIndex % 4 == 1;
	}
	else
	{
		check(ClipmapIndex == 3);
		return GlobalDistanceFieldUpdateIndex % 4 == 3;
	}
}

float ComputeClipmapExtent(int32 ClipmapIndex)
{
	return GAOInnerGlobalDFClipmapDistance * FMath::Pow(GAOGlobalDFClipmapDistanceExponent, ClipmapIndex);
}

void ComputeUpdateRegionsAndUpdateViewState(const FViewInfo& View, FGlobalDistanceFieldInfo& GlobalDistanceFieldInfo, int32 NumClipmaps, const TArray<FVector4>& PrimitiveModifiedBounds)
{
	GlobalDistanceFieldInfo.Clipmaps.AddZeroed(NumClipmaps);

	if (View.ViewState)
	{
		View.ViewState->GlobalDistanceFieldUpdateIndex++;

		if (View.ViewState->GlobalDistanceFieldUpdateIndex > 4)
		{
			View.ViewState->GlobalDistanceFieldUpdateIndex = 0;
		}

		for (int32 ClipmapIndex = 0; ClipmapIndex < NumClipmaps; ClipmapIndex++)
		{
			FGlobalDistanceFieldClipmapState& ClipmapViewState = View.ViewState->GlobalDistanceFieldClipmapState[ClipmapIndex];
			TRefCountPtr<IPooledRenderTarget>& RenderTarget = ClipmapViewState.VolumeTexture;

			bool bReallocated = false;

			if (!RenderTarget || RenderTarget->GetDesc().Extent.X != GAOGlobalDFResolution)
			{
				AllocateClipmapTexture(ClipmapIndex, RenderTarget);
				bReallocated = true;
			}

			FGlobalDistanceFieldClipmap& Clipmap = GlobalDistanceFieldInfo.Clipmaps[ClipmapIndex];

			const float Extent = ComputeClipmapExtent(ClipmapIndex);
			const float CellSize = (Extent * 2) / GAOGlobalDFResolution;

			// Accumulate primitive modifications in the viewstate in case we don't update the clipmap this frame
			ClipmapViewState.PrimitiveModifiedBounds.Append(PrimitiveModifiedBounds);

			if (ShouldUpdateClipmapThisFrame(ClipmapIndex, View.ViewState->GlobalDistanceFieldUpdateIndex)
				|| !View.ViewState->bIntializedGlobalDistanceFieldOrigins
				|| bReallocated)
			{
				const FVector NewCenter = View.ViewMatrices.ViewOrigin;

				FIntVector GridCenter;
				GridCenter.X = FMath::FloorToInt(NewCenter.X / CellSize);
				GridCenter.Y = FMath::FloorToInt(NewCenter.Y / CellSize);
				GridCenter.Z = FMath::FloorToInt(NewCenter.Z / CellSize);

				const FVector SnappedCenter = FVector(GridCenter) * CellSize;
				const FBox ClipmapBounds(SnappedCenter - Extent, SnappedCenter + Extent);

				bool bUsePartialUpdates = GAOGlobalDistanceFieldPartialUpdates
					&& View.ViewState->bIntializedGlobalDistanceFieldOrigins 
					&& !bReallocated
					// Only use partial updates with small numbers of primitive modifications
					&& ClipmapViewState.PrimitiveModifiedBounds.Num() < 100;

				if (bUsePartialUpdates)
				{
					FIntVector Movement = GridCenter - ClipmapViewState.LastPartialUpdateOrigin;

					// Add an update region for each potential axis of camera movement
					AddUpdateRegionForAxis(Movement, ClipmapBounds, CellSize, 0, Clipmap.UpdateRegions);
					AddUpdateRegionForAxis(Movement, ClipmapBounds, CellSize, 1, Clipmap.UpdateRegions);
					AddUpdateRegionForAxis(Movement, ClipmapBounds, CellSize, 2, Clipmap.UpdateRegions);

					for (int32 BoundsIndex = 0; BoundsIndex < ClipmapViewState.PrimitiveModifiedBounds.Num(); BoundsIndex++)
					{
						// Add an update region for each primitive that has been modified
						AddUpdateRegionForPrimitive(ClipmapViewState.PrimitiveModifiedBounds[BoundsIndex], ClipmapBounds, CellSize, Clipmap.UpdateRegions);
					}

					int32 TotalTexelsBeingUpdated = 0;

					// Trim fully contained update regions
					for (int32 UpdateRegionIndex = 0; UpdateRegionIndex < Clipmap.UpdateRegions.Num(); UpdateRegionIndex++)
					{
						const FVolumeUpdateRegion& UpdateRegion = Clipmap.UpdateRegions[UpdateRegionIndex];
						bool bCompletelyContained = false;

						for (int32 OtherUpdateRegionIndex = 0; OtherUpdateRegionIndex < Clipmap.UpdateRegions.Num(); OtherUpdateRegionIndex++)
						{
							if (UpdateRegionIndex != OtherUpdateRegionIndex)
							{
								const FVolumeUpdateRegion& OtherUpdateRegion = Clipmap.UpdateRegions[OtherUpdateRegionIndex];

								if (OtherUpdateRegion.Bounds.IsInsideOrOn(UpdateRegion.Bounds.Min)
									&& OtherUpdateRegion.Bounds.IsInsideOrOn(UpdateRegion.Bounds.Max))
								{
									bCompletelyContained = true;
									break;
								}
							}
						}

						if (bCompletelyContained)
						{
							Clipmap.UpdateRegions.RemoveAt(UpdateRegionIndex);
							UpdateRegionIndex--;
						}
						else
						{
							TotalTexelsBeingUpdated += UpdateRegion.CellsSize.X * UpdateRegion.CellsSize.Y * UpdateRegion.CellsSize.Z;
						}
					}

					if (TotalTexelsBeingUpdated > GAOGlobalDFResolution * GAOGlobalDFResolution * GAOGlobalDFResolution)
					{
						// Fall back to a full update if the partial updates were going to do more work
						bUsePartialUpdates = false;
						Clipmap.UpdateRegions.Reset();
					}
				}
		
				if (!bUsePartialUpdates)
				{
					FVolumeUpdateRegion UpdateRegion;
					UpdateRegion.Bounds = ClipmapBounds;
					UpdateRegion.CellsSize = FIntVector(GAOGlobalDFResolution);
					Clipmap.UpdateRegions.Add(UpdateRegion);

					// Store the location of the full update
					ClipmapViewState.FullUpdateOrigin = GridCenter;
					View.ViewState->bIntializedGlobalDistanceFieldOrigins = true;
				}

				ClipmapViewState.PrimitiveModifiedBounds.Reset();
				ClipmapViewState.LastPartialUpdateOrigin = GridCenter;
			}

			const FVector Center = FVector(ClipmapViewState.LastPartialUpdateOrigin) * CellSize;

			// Setup clipmap properties from view state exclusively, so we can skip updating on some frames
			Clipmap.RenderTarget = ClipmapViewState.VolumeTexture;
			Clipmap.Bounds = FBox(Center - Extent, Center + Extent);
			// Scroll offset so the contents of the global distance field don't have to be moved as the camera moves around, only updated in slabs
			Clipmap.ScrollOffset = FVector(ClipmapViewState.LastPartialUpdateOrigin - ClipmapViewState.FullUpdateOrigin) * CellSize;
		}
	}
	else
	{
		for (int32 ClipmapIndex = 0; ClipmapIndex < NumClipmaps; ClipmapIndex++)
		{
			FGlobalDistanceFieldClipmap& Clipmap = GlobalDistanceFieldInfo.Clipmaps[ClipmapIndex];
			AllocateClipmapTexture(ClipmapIndex, Clipmap.RenderTarget);
			Clipmap.ScrollOffset = FVector::ZeroVector;

			const float Extent = ComputeClipmapExtent(ClipmapIndex);
			FVector Center = View.ViewMatrices.ViewOrigin;

			const float CellSize = (Extent * 2) / GAOGlobalDFResolution;

			FIntVector GridCenter;
			GridCenter.X = FMath::FloorToInt(Center.X / CellSize);
			GridCenter.Y = FMath::FloorToInt(Center.Y / CellSize);
			GridCenter.Z = FMath::FloorToInt(Center.Z / CellSize);

			Center = FVector(GridCenter) * CellSize;

			FBox ClipmapBounds(Center - Extent, Center + Extent);
			Clipmap.Bounds = ClipmapBounds;

			FVolumeUpdateRegion UpdateRegion;
			UpdateRegion.Bounds = ClipmapBounds;
			UpdateRegion.CellsSize = FIntVector(GAOGlobalDFResolution);
			Clipmap.UpdateRegions.Add(UpdateRegion);
		}
	}
}

/** 
 * Updates the global distance field for a view.  
 * Typically issues updates for just the newly exposed regions of the volume due to camera movement.
 * In the worst case of a camera cut or large distance field scene changes, a full update of the global distance field will be done.
 **/
void UpdateGlobalDistanceFieldVolume(
	FRHICommandListImmediate& RHICmdList, 
	const FViewInfo& View, 
	const FScene* Scene, 
	float MaxOcclusionDistance, 
	FGlobalDistanceFieldInfo& GlobalDistanceFieldInfo)
{
	if (Scene->DistanceFieldSceneData.NumObjectsInBuffer > 0)
	{
		ComputeUpdateRegionsAndUpdateViewState(View, GlobalDistanceFieldInfo, GMaxGlobalDistanceFieldClipmaps, Scene->DistanceFieldSceneData.PrimitiveModifiedBounds);

		bool bHasUpdateRegions = false;

		for (int32 ClipmapIndex = 0; ClipmapIndex < GlobalDistanceFieldInfo.Clipmaps.Num(); ClipmapIndex++)
		{
			bHasUpdateRegions = bHasUpdateRegions || GlobalDistanceFieldInfo.Clipmaps[ClipmapIndex].UpdateRegions.Num() > 0;
		}

		if (bHasUpdateRegions && GAOUpdateGlobalDistanceField)
		{
			SCOPED_DRAW_EVENT(RHICmdList, UpdateGlobalDistanceFieldVolume);

			if (GGlobalDistanceFieldCulledObjectBuffers.Buffers.MaxObjects < Scene->DistanceFieldSceneData.NumObjectsInBuffer
				|| GGlobalDistanceFieldCulledObjectBuffers.Buffers.MaxObjects > 3 * Scene->DistanceFieldSceneData.NumObjectsInBuffer)
			{
				GGlobalDistanceFieldCulledObjectBuffers.Buffers.MaxObjects = Scene->DistanceFieldSceneData.NumObjectsInBuffer * 5 / 4;
				GGlobalDistanceFieldCulledObjectBuffers.Buffers.Release();
				GGlobalDistanceFieldCulledObjectBuffers.Buffers.Initialize();
			}

			const uint32 MaxCullGridDimension = GAOGlobalDFResolution / GCullGridTileSize;

			if (GObjectGridBuffers.GridDimension != MaxCullGridDimension)
			{
				GObjectGridBuffers.GridDimension = MaxCullGridDimension;
				GObjectGridBuffers.UpdateRHI();
			}

			for (int32 ClipmapIndex = 0; ClipmapIndex < GlobalDistanceFieldInfo.Clipmaps.Num(); ClipmapIndex++)
			{
				SCOPED_DRAW_EVENT(RHICmdList, Clipmap);

				FGlobalDistanceFieldClipmap& Clipmap = GlobalDistanceFieldInfo.Clipmaps[ClipmapIndex];

				for (int32 UpdateRegionIndex = 0; UpdateRegionIndex < Clipmap.UpdateRegions.Num(); UpdateRegionIndex++)
				{
					const FVolumeUpdateRegion& UpdateRegion = Clipmap.UpdateRegions[UpdateRegionIndex];

					{
						SCOPED_DRAW_EVENT(RHICmdList, GridCull);

						// Cull the global objects to the volume being updated
						{
							uint32 ClearValues[4] = { 0 };
							RHICmdList.ClearUAV(GGlobalDistanceFieldCulledObjectBuffers.Buffers.ObjectIndirectArguments.UAV, ClearValues);

							TShaderMapRef<FCullObjectsForVolumeCS> ComputeShader(View.ShaderMap);
							RHICmdList.SetComputeShader(ComputeShader->GetComputeShader());
							const FVector4 VolumeBounds(UpdateRegion.Bounds.GetCenter(), UpdateRegion.Bounds.GetExtent().Size());
							ComputeShader->SetParameters(RHICmdList, Scene, View, MaxOcclusionDistance, VolumeBounds);

							DispatchComputeShader(RHICmdList, *ComputeShader, FMath::DivideAndRoundUp<uint32>(Scene->DistanceFieldSceneData.NumObjectsInBuffer, CullObjectsGroupSize), 1, 1);
							ComputeShader->UnsetParameters(RHICmdList);
						}

						// Further cull the objects into a low resolution grid
						{
							TShaderMapRef<FCullObjectsToGridCS> ComputeShader(View.ShaderMap);
							RHICmdList.SetComputeShader(ComputeShader->GetComputeShader());
							ComputeShader->SetParameters(RHICmdList, Scene, View, MaxOcclusionDistance, GlobalDistanceFieldInfo, ClipmapIndex, UpdateRegion);

							const uint32 NumGroupsX = FMath::DivideAndRoundUp<int32>(UpdateRegion.CellsSize.X, GCullGridTileSize);
							const uint32 NumGroupsY = FMath::DivideAndRoundUp<int32>(UpdateRegion.CellsSize.Y, GCullGridTileSize);
							const uint32 NumGroupsZ = FMath::DivideAndRoundUp<int32>(UpdateRegion.CellsSize.Z, GCullGridTileSize); 

							DispatchComputeShader(RHICmdList, *ComputeShader, NumGroupsX, NumGroupsY, NumGroupsZ);
							ComputeShader->UnsetParameters(RHICmdList);
						}
					}

					// Further cull the objects to the dispatch tile and composite the global distance field by computing the min distance from intersecting per-object distance fields
					{
						SCOPED_DRAW_EVENT(RHICmdList, TileCullAndComposite);
						TShaderMapRef<FCompositeObjectDistanceFieldsCS> ComputeShader(View.ShaderMap);
						RHICmdList.SetComputeShader(ComputeShader->GetComputeShader());
						ComputeShader->SetParameters(RHICmdList, Scene, View, MaxOcclusionDistance, GlobalDistanceFieldInfo, ClipmapIndex, UpdateRegion);

						//@todo - match typical update sizes.  Camera movement creates narrow slabs.
						const uint32 NumGroupsX = FMath::DivideAndRoundUp<int32>(UpdateRegion.CellsSize.X, CompositeTileSize);
						const uint32 NumGroupsY = FMath::DivideAndRoundUp<int32>(UpdateRegion.CellsSize.Y, CompositeTileSize);
						const uint32 NumGroupsZ = FMath::DivideAndRoundUp<int32>(UpdateRegion.CellsSize.Z, CompositeTileSize);

						DispatchComputeShader(RHICmdList, *ComputeShader, NumGroupsX, NumGroupsY, NumGroupsZ);
						ComputeShader->UnsetParameters(RHICmdList);
					}
				}
			}
		}
	}
}

void ListGlobalDistanceFieldMemory()
{
	UE_LOG(LogTemp, Log, TEXT("   Global DF culled objects %.3fMb"), (GGlobalDistanceFieldCulledObjectBuffers.Buffers.GetSizeBytes() + GObjectGridBuffers.GetSizeBytes()) / 1024.0f / 1024.0f);
}