// Copyright 1998-2014 Epic Games, Inc. All Rights Reserved.

#pragma once
#include "Engine/NavigationObjectBase.h"
#include "PlayerStart.generated.h"

/** 
 *	This class indicates a location where a player can spawn when the game begins
 *	
 *	@see https://docs.unrealengine.com/latest/INT/Engine/Actors/PlayerStart/
 */
UCLASS(ClassGroup=Common, hidecategories=Collision)
class ENGINE_API APlayerStart : public ANavigationObjectBase
{
	/** To take more control over PlayerStart selection, you can override the virtual AGameMode::FindPlayerStart and AGameMode::ChoosePlayerStart function */

	GENERATED_UCLASS_BODY()

	/** Used when searching for which playerstart to use. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Object)
	FName PlayerStartTag;

	/** Arrow component to indicate forward direction of start */
#if WITH_EDITORONLY_DATA
private_subobject:
	DEPRECATED_FORGAME(4.6, "ArrowComponent should not be accessed directly, please use GetArrowComponent() function instead. ArrowComponent will soon be private and your code will not compile.")
	UPROPERTY()
	class UArrowComponent* ArrowComponent;
public:
#endif

	// Begin AActor interface
	virtual void PostInitializeComponents() override;	
	virtual void PostUnregisterAllComponents() override;
	// End AActor interface

#if WITH_EDITORONLY_DATA
	/** Returns ArrowComponent subobject **/
	class UArrowComponent* GetArrowComponent() const;
#endif
};



