// Copyright 1998-2014 Epic Games, Inc. All Rights Reserved.

#pragma once

#include "EditorTutorial.h"
#include "TutorialStateSettings.generated.h"

/** Track the progress of an individual tutorial */
USTRUCT()
struct FTutorialProgress
{
	GENERATED_USTRUCT_BODY()

	FTutorialProgress()
	{
		bUserDismissedThisSession = false;
	}

	UPROPERTY()
	FStringClassReference Tutorial;

	UPROPERTY()
	int32 CurrentStage;

	UPROPERTY()
	bool bUserDismissed;

	/** Non-persistent flag indicating the user dismissed this tutorial */
	bool bUserDismissedThisSession;
};

/** Tutorial settings used to track completion state */
UCLASS(config=EditorGameAgnostic)
class UTutorialStateSettings : public UObject
{
	GENERATED_UCLASS_BODY()

	UPROPERTY(Config)
	TArray<FTutorialProgress> TutorialsProgress;

	/** UObject interface */
	virtual void PostInitProperties() override;

	/** Get the recorded progress of the pass-in tutorial */
	int32 GetProgress(UEditorTutorial* InTutorial, bool& bOutHaveSeenTutorial) const;

	/** Check if we have seen the passed-in tutorial before */
	bool HaveSeenTutorial(UEditorTutorial* InTutorial) const;

	/** Check if completed the passed in tutorial (i.e. seen all of its stages) */
	bool HaveCompletedTutorial(UEditorTutorial* InTutorial) const;

	/** Flag a tutorial as dismissed */
	void DismissTutorial(UEditorTutorial* InTutorial, bool bDismissAcrossSessions);

	/** Check if a tutorial has been dismissed */
	bool IsTutorialDismissed(UEditorTutorial* InTutorial) const;

	/** Record the progress of the passed-in tutorial */
	void RecordProgress(UEditorTutorial* InTutorial, int32 CurrentStage);

	/** Save the progress of all our tutorials */
	void SaveProgress();

private:
	/** Recorded progress */
	TMap<UEditorTutorial*, FTutorialProgress> ProgressMap;
};