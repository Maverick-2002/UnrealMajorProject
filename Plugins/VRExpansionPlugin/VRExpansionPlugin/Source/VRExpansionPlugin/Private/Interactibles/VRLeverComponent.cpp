// Copyright 1998-2016 Epic Games, Inc. All Rights Reserved.

#include "Interactibles/VRLeverComponent.h"
#include "Net/UnrealNetwork.h"

  //=============================================================================
UVRLeverComponent::UVRLeverComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	this->SetGenerateOverlapEvents(true);
	this->PrimaryComponentTick.bStartWithTickEnabled = false;
	PrimaryComponentTick.bCanEverTick = true;

	bRepGameplayTags = false;

	// Defaulting these true so that they work by default in networked environments
	bReplicateMovement = true;

	MovementReplicationSetting = EGripMovementReplicationSettings::ForceClientSideMovement;
	BreakDistance = 100.0f;
	Stiffness = 1500.0f;
	Damping = 200.0f;

	HandleData = nullptr;
	//SceneIndex = 0;

	bIsPhysicsLever = false;
	ParentComponent = nullptr;
	LeverRotationAxis = EVRInteractibleLeverAxis::Axis_X;
	
	LeverLimitNegative = 0.0f;
	LeverLimitPositive = 90.0f;
	bLeverState = false;
	LeverTogglePercentage = 0.8f;

	LastDeltaAngle = 0.0f;
	FullCurrentAngle = 0.0f;

	LeverReturnTypeWhenReleased = EVRInteractibleLeverReturnType::ReturnToZero;
	LeverReturnSpeed = 50.0f;

	MomentumAtDrop = 0.0f;
	LeverMomentumFriction = 5.0f;
	MaxLeverMomentum = 180.0f;
	FramesToAverage = 3;

	bBlendAxisValuesByAngleThreshold = false;
	AngleThreshold = 90.0f;

	LastLeverAngle = 0.0f;

	bSendLeverEventsDuringLerp = false;

	InitialRelativeTransform = FTransform::Identity;
	InitialInteractorLocation = FVector::ZeroVector;
	InteractorOffsetTransform = FTransform::Identity;
	AllCurrentLeverAngles = FRotator::ZeroRotator;
	InitialGripRot = 0.0f;
	qRotAtGrab = FQuat::Identity;
	bIsLerping = false;
	bUngripAtTargetRotation = false;
	bDenyGripping = false;

	bIsLocked = false;
	bAutoDropWhenLocked = true;

	GripPriority = 1;

	// Set to only overlap with things so that its not ruined by touching over actors
	this->SetCollisionResponseToAllChannels(ECollisionResponse::ECR_Overlap);
}

//=============================================================================
UVRLeverComponent::~UVRLeverComponent()
{
}


void UVRLeverComponent::GetLifetimeReplicatedProps(TArray< class FLifetimeProperty > & OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(UVRLeverComponent, InitialRelativeTransform);
	//DOREPLIFETIME_CONDITION(UVRLeverComponent, bIsLerping, COND_InitialOnly);

	DOREPLIFETIME(UVRLeverComponent, bRepGameplayTags);
	DOREPLIFETIME(UVRLeverComponent, bReplicateMovement);
	DOREPLIFETIME_CONDITION(UVRLeverComponent, GameplayTags, COND_Custom);
}

void UVRLeverComponent::PreReplication(IRepChangedPropertyTracker & ChangedPropertyTracker)
{
	Super::PreReplication(ChangedPropertyTracker);

	// Replicate the levers initial transform if we are replicating movement
	//DOREPLIFETIME_ACTIVE_OVERRIDE(UVRLeverComponent, InitialRelativeTransform, bReplicateMovement);

	// Don't replicate if set to not do it
	DOREPLIFETIME_ACTIVE_OVERRIDE(UVRLeverComponent, GameplayTags, bRepGameplayTags);

	DOREPLIFETIME_ACTIVE_OVERRIDE_PRIVATE_PROPERTY(USceneComponent, RelativeLocation, bReplicateMovement);
	DOREPLIFETIME_ACTIVE_OVERRIDE_PRIVATE_PROPERTY(USceneComponent, RelativeRotation, bReplicateMovement);
	DOREPLIFETIME_ACTIVE_OVERRIDE_PRIVATE_PROPERTY(USceneComponent, RelativeScale3D,	bReplicateMovement);
}

void UVRLeverComponent::OnRegister()
{
	Super::OnRegister();
	ResetInitialLeverLocation(); // Load the original lever location
}

void UVRLeverComponent::BeginPlay()
{
	// Call the base class 
	Super::BeginPlay();
	ReCalculateCurrentAngle(true);
	bOriginalReplicatesMovement = bReplicateMovement;
}

void UVRLeverComponent::TickComponent(float DeltaTime, enum ELevelTick TickType, FActorComponentTickFunction *ThisTickFunction)
{
	// Call supers tick (though I don't think any of the base classes to this actually implement it)
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	bool bWasLerping = bIsLerping;

	// If we are locked then end the lerp, no point
	if (bIsLocked)
	{

		if (bWasLerping)
		{
			bIsLerping = false;

			// If we start lerping while locked, just end it
			OnLeverFinishedLerping.Broadcast(CurrentLeverAngle);
			ReceiveLeverFinishedLerping(CurrentLeverAngle);
		}

		return;
	}

	if (bIsLerping)
	{
		FTransform CurRelativeTransform = this->GetComponentTransform().GetRelativeTransform(UVRInteractibleFunctionLibrary::Interactible_GetCurrentParentTransform(this));

		switch (LeverRotationAxis)
		{
		case EVRInteractibleLeverAxis::Axis_X:
		case EVRInteractibleLeverAxis::Axis_Y:
		case EVRInteractibleLeverAxis::Axis_Z:
		{
			LerpAxis(FullCurrentAngle, DeltaTime);
		}break;
		case EVRInteractibleLeverAxis::Axis_XY:
		case EVRInteractibleLeverAxis::FlightStick_XY:
		{
			// Only supporting LerpToZero with this mode currently
			FQuat LerpedQuat = FMath::QInterpConstantTo(CurRelativeTransform.GetRelativeTransform(InitialRelativeTransform).GetRotation(), FQuat::Identity, DeltaTime, FMath::DegreesToRadians(LeverReturnSpeed));

			if (LerpedQuat.IsIdentity())
			{
				this->SetComponentTickEnabled(false);
				bIsLerping = false;
				bReplicateMovement = bOriginalReplicatesMovement;
				this->SetRelativeRotation(InitialRelativeTransform.Rotator());
			}
			else
			{
				this->SetRelativeRotation((FTransform(LerpedQuat) * InitialRelativeTransform).GetRotation());
			}
		}break;
		default:break;
		}
	}

	FTransform CurrentRelativeTransform = this->GetComponentTransform().GetRelativeTransform(UVRInteractibleFunctionLibrary::Interactible_GetCurrentParentTransform(this));
	CalculateCurrentAngle(CurrentRelativeTransform);


	if (!bWasLerping && LeverReturnTypeWhenReleased == EVRInteractibleLeverReturnType::RetainMomentum)
	{
		// Rolling average across num samples
		MomentumAtDrop -= MomentumAtDrop / FramesToAverage;
		MomentumAtDrop += ((FullCurrentAngle - LastLeverAngle) / DeltaTime) / FramesToAverage;

		MomentumAtDrop = FMath::Min(MaxLeverMomentum, MomentumAtDrop);

		LastLeverAngle = FullCurrentAngle;
	}

	// Check for events and set current state and check for auto drop
	ProccessCurrentState(bWasLerping, true, true);

	// If the lerping state changed from the above
	if (bWasLerping && !bIsLerping)
	{
		OnLeverFinishedLerping.Broadcast(CurrentLeverAngle);
		ReceiveLeverFinishedLerping(CurrentLeverAngle);
	}
}

void UVRLeverComponent::ProccessCurrentState(bool bWasLerping, bool bThrowEvents, bool bCheckAutoDrop)
{
	bool bNewLeverState = (!FMath::IsNearlyZero(LeverLimitNegative) && FullCurrentAngle <= -(LeverLimitNegative * LeverTogglePercentage)) || (!FMath::IsNearlyZero(LeverLimitPositive) && FullCurrentAngle >= (LeverLimitPositive * LeverTogglePercentage));
	//if (FMath::Abs(CurrentLeverAngle) >= LeverLimit  )
	if (bNewLeverState != bLeverState)
	{
		bLeverState = bNewLeverState;

		if (bThrowEvents && (bSendLeverEventsDuringLerp || !bWasLerping))
		{
			ReceiveLeverStateChanged(bLeverState, FullCurrentAngle >= 0.0f ? EVRInteractibleLeverEventType::LeverPositive : EVRInteractibleLeverEventType::LeverNegative, CurrentLeverAngle, FullCurrentAngle);
			OnLeverStateChanged.Broadcast(bLeverState, FullCurrentAngle >= 0.0f ? EVRInteractibleLeverEventType::LeverPositive : EVRInteractibleLeverEventType::LeverNegative, CurrentLeverAngle, FullCurrentAngle);
		}

		if (bCheckAutoDrop)
		{
			if (!bWasLerping && bUngripAtTargetRotation && bLeverState && HoldingGrip.IsValid())
			{
				FBPActorGripInformation GripInformation;
				EBPVRResultSwitch result;
				HoldingGrip.HoldingController->GetGripByID(GripInformation, HoldingGrip.GripID, result);
				if (result == EBPVRResultSwitch::OnSucceeded && HoldingGrip.HoldingController->HasGripAuthority(GripInformation))
				{
					HoldingGrip.HoldingController->DropObjectByInterface(this, HoldingGrip.GripID);
				}
			}
		}
	}
}

void UVRLeverComponent::OnUnregister()
{
	DestroyConstraint();
	Super::OnUnregister();
}

bool UVRLeverComponent::CheckAutoDrop(UGripMotionControllerComponent* GrippingController, const FBPActorGripInformation& GripInformation)
{
	// Converted to a relative value now so it should be correct
	if (BreakDistance > 0.f && GrippingController->HasGripAuthority(GripInformation) && FVector::DistSquared(InitialInteractorDropLocation, this->GetComponentTransform().InverseTransformPosition(GrippingController->GetPivotLocation())) >= FMath::Square(BreakDistance))
	{
		GrippingController->DropObjectByInterface(this, HoldingGrip.GripID);
		return true;
	}

	return false;
}

void UVRLeverComponent::TickGrip_Implementation(UGripMotionControllerComponent * GrippingController, const FBPActorGripInformation & GripInformation, float DeltaTime) 
{
	if (bIsLocked)
	{
		if (bAutoDropWhenLocked)
		{
			// Check if we should auto drop
			CheckAutoDrop(GrippingController, GripInformation);
		}

		return;
	}

	// Handle manual tracking here
	FTransform ParentTransform = UVRInteractibleFunctionLibrary::Interactible_GetCurrentParentTransform(this);
	FTransform CurrentRelativeTransform = InitialRelativeTransform * ParentTransform;
	FTransform PivotTransform = GrippingController->GetPivotTransform();
	FVector CurInteractorLocation = (InteractorOffsetTransform * PivotTransform).GetRelativeTransform(CurrentRelativeTransform).GetTranslation();

	switch (LeverRotationAxis)
	{
	case EVRInteractibleLeverAxis::Axis_XY:
	case EVRInteractibleLeverAxis::FlightStick_XY:
	{
		FRotator Rot;

		FVector nAxis;
		float nAngle = 0.0f;

		FQuat::FindBetweenVectors(qRotAtGrab.UnrotateVector(InitialInteractorLocation), CurInteractorLocation).ToAxisAndAngle(nAxis, nAngle);
		float MaxAngle = FMath::DegreesToRadians(LeverLimitPositive);

		bool bWasClamped = nAngle > MaxAngle;
		nAngle = FMath::Clamp(nAngle, 0.0f, MaxAngle);
		Rot = FQuat(nAxis, nAngle).Rotator();

		if (LeverRotationAxis == EVRInteractibleLeverAxis::FlightStick_XY)
		{
			// Store our projected relative transform
			FTransform CalcTransform = (FTransform(Rot) * InitialRelativeTransform);

			// Fixup yaw if this is a flight stick

			if (bWasClamped)
			{
				// If we clamped the angle due to limits then lets re-project the hand back to get the correct facing again
				// This is only when things have been clamped to avoid the extra calculations
				FTransform NewPivTrans = PivotTransform.GetRelativeTransform((CalcTransform * ParentTransform));
				CurInteractorLocation = NewPivTrans.GetTranslation();

				FVector OffsetVal = CurInteractorLocation + NewPivTrans.GetRotation().RotateVector(InteractorOffsetTransform.GetTranslation());
				OffsetVal.Z = 0;

				CurInteractorLocation -= OffsetVal;
			}
			else
			{
				CurInteractorLocation = (CalcTransform * ParentTransform).InverseTransformPosition(GrippingController->GetPivotLocation());
			}

			float CurrentLeverYawAngle = UVRInteractibleFunctionLibrary::GetAtan2Angle(EVRInteractibleAxis::Axis_Z, CurInteractorLocation, InitialGripRot);
			FQuat newLocalRot = CalcTransform.GetRotation() * FQuat(FVector::UpVector, FMath::DegreesToRadians(CurrentLeverYawAngle));
			this->SetRelativeRotation(newLocalRot.Rotator());
		}
		else
		{
			this->SetRelativeRotation((FTransform(Rot) * InitialRelativeTransform).Rotator());
		}
	}
	break;
	case EVRInteractibleLeverAxis::Axis_X:
	case EVRInteractibleLeverAxis::Axis_Y:
	case EVRInteractibleLeverAxis::Axis_Z:
	{
		float DeltaAngle = CalcAngle(LeverRotationAxis, CurInteractorLocation);
		LastDeltaAngle = DeltaAngle;
		FTransform CalcTransform = (FTransform(UVRInteractibleFunctionLibrary::SetAxisValueRot((EVRInteractibleAxis)LeverRotationAxis, DeltaAngle, FRotator::ZeroRotator)) * InitialRelativeTransform);
		this->SetRelativeRotation(CalcTransform.Rotator());
	}break;
	default:break;
	}

	// Recalc current angle
	CurrentRelativeTransform = this->GetComponentTransform().GetRelativeTransform(UVRInteractibleFunctionLibrary::Interactible_GetCurrentParentTransform(this));
	CalculateCurrentAngle(CurrentRelativeTransform);

	// Check for events and set current state and check for auto drop
	ProccessCurrentState(bIsLerping, true, true);

	// Check if we should auto drop
	CheckAutoDrop(GrippingController, GripInformation);
}

void UVRLeverComponent::OnGrip_Implementation(UGripMotionControllerComponent * GrippingController, const FBPActorGripInformation & GripInformation) 
{
	ParentComponent = this->GetAttachParent();

	if (bIsPhysicsLever)
	{
		SetupConstraint();
	}
	else
	{
		FTransform CurrentRelativeTransform = InitialRelativeTransform * UVRInteractibleFunctionLibrary::Interactible_GetCurrentParentTransform(this);
			
		// This lets me use the correct original location over the network without changes
		FTransform ReversedRelativeTransform = FTransform(GripInformation.RelativeTransform.ToInverseMatrixWithScale());
		FTransform CurrentTransform = this->GetComponentTransform();
		FTransform RelativeToGripTransform = FTransform::Identity;

		if (LeverRotationAxis == EVRInteractibleLeverAxis::FlightStick_XY)
		{
			// Offset the grip to the same height on the cross axis and centered on the lever
			FVector InitialInteractorOffset = ReversedRelativeTransform.GetTranslation();
			FTransform InitTrans = ReversedRelativeTransform;
			InitialInteractorOffset.X = 0;
			InitialInteractorOffset.Y = 0;
			InteractorOffsetTransform = ReversedRelativeTransform;
			InteractorOffsetTransform.AddToTranslation(-InitialInteractorOffset);
			InteractorOffsetTransform = FTransform(InteractorOffsetTransform.ToInverseMatrixWithScale());

			InitialInteractorOffset = ReversedRelativeTransform.GetTranslation();
			InitialInteractorOffset.Z = 0;

			InitTrans.AddToTranslation(-InitialInteractorOffset);
			RelativeToGripTransform = InitTrans * CurrentTransform;
		}
		else
		{
			RelativeToGripTransform = ReversedRelativeTransform * CurrentTransform;
			InteractorOffsetTransform = FTransform::Identity;
		}

		InitialInteractorLocation = CurrentRelativeTransform.InverseTransformPosition(RelativeToGripTransform.GetTranslation());
		InitialInteractorDropLocation = ReversedRelativeTransform.GetTranslation();

		switch (LeverRotationAxis)
		{
		case EVRInteractibleLeverAxis::Axis_XY:
		{
			qRotAtGrab = this->GetComponentTransform().GetRelativeTransform(CurrentRelativeTransform).GetRotation();
		}break;
		case EVRInteractibleLeverAxis::FlightStick_XY:
		{
			qRotAtGrab = this->GetComponentTransform().GetRelativeTransform(CurrentRelativeTransform).GetRotation();
			InitialGripRot = UVRInteractibleFunctionLibrary::GetAtan2Angle(EVRInteractibleAxis::Axis_Z, ReversedRelativeTransform.GetTranslation());
		}break;
		case EVRInteractibleLeverAxis::Axis_X:
		case EVRInteractibleLeverAxis::Axis_Y:
		{
			// Get our initial interactor rotation
			InitialGripRot = UVRInteractibleFunctionLibrary::GetAtan2Angle((EVRInteractibleAxis)LeverRotationAxis, InitialInteractorLocation);
		}break;

		case EVRInteractibleLeverAxis::Axis_Z:
		{
			// Get our initial interactor rotation
			InitialGripRot = UVRInteractibleFunctionLibrary::GetAtan2Angle((EVRInteractibleAxis)LeverRotationAxis, InitialInteractorLocation);
		}break;

		default:break;
		}
		
		// Get out current rotation at grab
		RotAtGrab = UVRInteractibleFunctionLibrary::GetDeltaAngleFromTransforms((EVRInteractibleAxis)LeverRotationAxis, CurrentRelativeTransform, CurrentTransform);
	}

	LastLeverAngle = CurrentLeverAngle;
	bIsLerping = false;
	bIsInFirstTick = true;
	MomentumAtDrop = 0.0f;

	this->SetComponentTickEnabled(true);

	OnGripped.Broadcast(GrippingController, GripInformation);
}

void UVRLeverComponent::OnGripRelease_Implementation(UGripMotionControllerComponent * ReleasingController, const FBPActorGripInformation & GripInformation, bool bWasSocketed) 
{
	if(bIsPhysicsLever)
	{
		DestroyConstraint();
		FAttachmentTransformRules AttachRules(EAttachmentRule::KeepWorld, true);
		this->AttachToComponent(ParentComponent.Get(), AttachRules);
	}

	if (LeverReturnTypeWhenReleased != EVRInteractibleLeverReturnType::Stay)
	{		
		bIsLerping = true;
		this->SetComponentTickEnabled(true);
		if (MovementReplicationSetting != EGripMovementReplicationSettings::ForceServerSideMovement)
			bReplicateMovement = false;
	}
	else
	{
		this->SetComponentTickEnabled(false);
		bReplicateMovement = bOriginalReplicatesMovement;
	}

	OnDropped.Broadcast(ReleasingController, GripInformation, bWasSocketed);
}

void UVRLeverComponent::SetGripPriority(int NewGripPriority)
{
	GripPriority = NewGripPriority;
}

void UVRLeverComponent::SetIsLocked(bool bNewLockedState)
{
	bIsLocked = bNewLockedState;
}

void UVRLeverComponent::OnChildGrip_Implementation(UGripMotionControllerComponent * GrippingController, const FBPActorGripInformation & GripInformation) {}
void UVRLeverComponent::OnChildGripRelease_Implementation(UGripMotionControllerComponent * ReleasingController, const FBPActorGripInformation & GripInformation, bool bWasSocketed) {}
void UVRLeverComponent::OnSecondaryGrip_Implementation(UGripMotionControllerComponent * GripOwningController, USceneComponent * SecondaryGripComponent, const FBPActorGripInformation & GripInformation) {}
void UVRLeverComponent::OnSecondaryGripRelease_Implementation(UGripMotionControllerComponent * GripOwningController, USceneComponent * ReleasingSecondaryGripComponent, const FBPActorGripInformation & GripInformation) {}
void UVRLeverComponent::OnUsed_Implementation() {}
void UVRLeverComponent::OnEndUsed_Implementation() {}
void UVRLeverComponent::OnSecondaryUsed_Implementation() {}
void UVRLeverComponent::OnEndSecondaryUsed_Implementation() {}
void UVRLeverComponent::OnInput_Implementation(FKey Key, EInputEvent KeyEvent) {}
bool UVRLeverComponent::RequestsSocketing_Implementation(USceneComponent *& ParentToSocketTo, FName & OptionalSocketName, FTransform_NetQuantize & RelativeTransform) { return false; }

bool UVRLeverComponent::DenyGripping_Implementation()
{
	return bDenyGripping;
}

EGripInterfaceTeleportBehavior UVRLeverComponent::TeleportBehavior_Implementation()
{
	return EGripInterfaceTeleportBehavior::DropOnTeleport;
}

bool UVRLeverComponent::SimulateOnDrop_Implementation()
{
	return false;
}

/*EGripCollisionType UVRLeverComponent::SlotGripType_Implementation()
{
	if (bIsPhysicsLever)
		return EGripCollisionType::ManipulationGrip;
	else
		return EGripCollisionType::CustomGrip;
}

EGripCollisionType UVRLeverComponent::FreeGripType_Implementation()
{
	if (bIsPhysicsLever)
		return EGripCollisionType::ManipulationGrip;
	else
		return EGripCollisionType::CustomGrip;
}*/

EGripCollisionType UVRLeverComponent::GetPrimaryGripType_Implementation(bool bIsSlot)
{
	if (bIsPhysicsLever)
		return EGripCollisionType::ManipulationGrip;
	else
		return EGripCollisionType::CustomGrip;
}

ESecondaryGripType UVRLeverComponent::SecondaryGripType_Implementation()
{
	return ESecondaryGripType::SG_None;
}


EGripMovementReplicationSettings UVRLeverComponent::GripMovementReplicationType_Implementation()
{
	return MovementReplicationSetting;
}

EGripLateUpdateSettings UVRLeverComponent::GripLateUpdateSetting_Implementation()
{
	return EGripLateUpdateSettings::LateUpdatesAlwaysOff;
}

/*float UVRLeverComponent::GripStiffness_Implementation()
{
	return Stiffness;
}

float UVRLeverComponent::GripDamping_Implementation()
{
	return Damping;
}*/
void UVRLeverComponent::GetGripStiffnessAndDamping_Implementation(float &GripStiffnessOut, float &GripDampingOut)
{
	GripStiffnessOut = Stiffness;
	GripDampingOut = Damping;
}

FBPAdvGripSettings UVRLeverComponent::AdvancedGripSettings_Implementation()
{
	return FBPAdvGripSettings(GripPriority);
}

float UVRLeverComponent::GripBreakDistance_Implementation()
{
	return BreakDistance;
}

/*void UVRLeverComponent::ClosestSecondarySlotInRange_Implementation(FVector WorldLocation, bool & bHadSlotInRange, FTransform & SlotWorldTransform, UGripMotionControllerComponent * CallingController, FName OverridePrefix)
{
	bHadSlotInRange = false;
}

void UVRLeverComponent::ClosestPrimarySlotInRange_Implementation(FVector WorldLocation, bool & bHadSlotInRange, FTransform & SlotWorldTransform, UGripMotionControllerComponent * CallingController, FName OverridePrefix)
{
	bHadSlotInRange = false;
}*/

void UVRLeverComponent::ClosestGripSlotInRange_Implementation(FVector WorldLocation, bool bSecondarySlot, bool & bHadSlotInRange, FTransform & SlotWorldTransform, FName & SlotName, UGripMotionControllerComponent * CallingController, FName OverridePrefix)
{
	bHadSlotInRange = false;
}

bool UVRLeverComponent::AllowsMultipleGrips_Implementation()
{
	return false;
}

void UVRLeverComponent::IsHeld_Implementation(TArray<FBPGripPair> & CurHoldingControllers, bool & bCurIsHeld)
{
	CurHoldingControllers.Empty();
	if (HoldingGrip.IsValid())
	{
		CurHoldingControllers.Add(HoldingGrip);
		bCurIsHeld = bIsHeld;
	}
	else
	{
		bCurIsHeld = false;
	}
}

void UVRLeverComponent::SetHeld_Implementation(UGripMotionControllerComponent * NewHoldingController, uint8 GripID, bool bNewIsHeld)
{
	if (bNewIsHeld)
	{
		HoldingGrip = FBPGripPair(NewHoldingController, GripID);
		if (MovementReplicationSetting != EGripMovementReplicationSettings::ForceServerSideMovement)
		{
			if (!bIsHeld && !bIsLerping)
				bOriginalReplicatesMovement = bReplicateMovement;
			bReplicateMovement = false;
		}
	}
	else
	{
		HoldingGrip.Clear();
		if (MovementReplicationSetting != EGripMovementReplicationSettings::ForceServerSideMovement)
		{
			bReplicateMovement = bOriginalReplicatesMovement;
		}
	}

	bIsHeld = bNewIsHeld;
}

/*FBPInteractionSettings UVRLeverComponent::GetInteractionSettings_Implementation()
{
	return FBPInteractionSettings();
}*/

bool UVRLeverComponent::GetGripScripts_Implementation(TArray<UVRGripScriptBase*> & ArrayReference)
{
	return false;
}

bool UVRLeverComponent::DestroyConstraint()
{
#if WITH_PHYSX
	if (HandleData)
	{
		// use correct scene
		PxScene* PScene = HandleData->getScene();//GetPhysXSceneFromIndex(SceneIndex);
		if (PScene)
		{
			PScene->lockWrite();
			//SCOPED_SCENE_WRITE_LOCK(PScene);
			// Destroy joint.
			HandleData->release();
			PScene->unlockWrite();
		}

		HandleData = NULL;
		return true;
	}
	else
	{
		return false;
	}
#endif // WITH_PHYSX

	return true;
}

bool UVRLeverComponent::SetupConstraint()
{
#if WITH_PHYSX

	if (HandleData)
		return true;

	// Get the PxRigidDynamic that we want to grab.
	FBodyInstance* rBodyInstance = this->GetBodyInstance(NAME_None);
	if (!rBodyInstance)
	{
		return false;
	}


	FTransform A2Transform = FTransform::Identity;//GetComponentTransform().Inverse();
	if (ParentComponent.IsValid())
	{
		UPrimitiveComponent * PrimComp = Cast<UPrimitiveComponent>(ParentComponent.Get());

		if (PrimComp)
			A2Transform = PrimComp->GetComponentTransform();
	}

	float rotationalOffset = (LeverLimitPositive - LeverLimitNegative) / 2;
	FRotator AngularRotationOffset = UVRInteractibleFunctionLibrary::SetAxisValueRot((EVRInteractibleAxis)LeverRotationAxis, rotationalOffset, FRotator::ZeroRotator);
	FTransform RefFrame2 = FTransform(InitialRelativeTransform.GetRotation() * AngularRotationOffset.Quaternion(), A2Transform.InverseTransformPosition(GetComponentLocation()));

	// If we don't already have a handle - make one now.
	if (!HandleData)
	{
		FPhysicsCommand::ExecuteWrite(BodyInstance.ActorHandle, [&](const FPhysicsActorHandle& Actor)
			//ExecuteOnPxRigidDynamicReadWrite(rBodyInstance, [&](PxRigidDynamic* Actor)
		{
			if (PxRigidActor* PActor = FPhysicsInterface::GetPxRigidActor_AssumesLocked(Actor))
			{
				PxScene* Scene = PActor->getScene();
				PxD6Joint* NewJoint = NULL;
				PxRigidDynamic * ParentBody = NULL;

				if (ParentComponent.IsValid())
				{
					UPrimitiveComponent * PrimComp = Cast<UPrimitiveComponent>(ParentComponent.Get());

					if (PrimComp && PrimComp->BodyInstance.IsValidBodyInstance())
					{
						ParentBody = FPhysicsInterface::GetPxRigidDynamic_AssumesLocked(PrimComp->BodyInstance.ActorHandle);
						//ParentBody = PrimComp->BodyInstance.GetPxRigidDynamic_AssumesLocked();
					}
				}

				NewJoint = PxD6JointCreate(Scene->getPhysics(), ParentBody, U2PTransform(RefFrame2), PActor, PxTransform(PxIdentity));

				if (!NewJoint)
				{
					HandleData = NULL;
				}
				else
				{
					// No constraint instance
					NewJoint->userData = NULL; // don't need
					HandleData = NewJoint;

					// Remember the scene index that the handle joint/actor are in.
					FPhysScene* RBScene = FPhysxUserData::Get<FPhysScene>(Scene->userData);
					//const uint32 SceneType = rBodyInstance->UseAsyncScene(RBScene) ? PST_Async : PST_Sync;
					//SceneIndex = RBScene->PhysXSceneIndex[SceneType];

					// Pretty Much Unbreakable
					NewJoint->setBreakForce(PX_MAX_REAL, PX_MAX_REAL);
					//	NewJoint->setConstraintFlag(PxConstraintFlag::ePROJECTION, true);

					//	NewJoint->setConstraintFlag(PxConstraintFlag::eCOLLISION_ENABLED, false);

					PxConstraintFlags Flags = NewJoint->getConstraintFlags();

					// False flags
					//Flags |= PxConstraintFlag::ePROJECTION;
					Flags |= PxConstraintFlag::eCOLLISION_ENABLED;

					// True flags
					Flags &= ~PxConstraintFlag::ePROJECTION;

					NewJoint->setConstraintFlag(PxConstraintFlag::ePROJECTION, true);
					NewJoint->setProjectionAngularTolerance(FMath::DegreesToRadians(0.1f));
					NewJoint->setProjectionLinearTolerance(0.1f);
					NewJoint->setConstraintFlags(Flags);

					// Setting up the joint
					NewJoint->setMotion(PxD6Axis::eX, PxD6Motion::eLOCKED);
					NewJoint->setMotion(PxD6Axis::eY, PxD6Motion::eLOCKED);
					NewJoint->setMotion(PxD6Axis::eZ, PxD6Motion::eLOCKED);

					NewJoint->setMotion(PxD6Axis::eTWIST, LeverRotationAxis == EVRInteractibleLeverAxis::Axis_X || LeverRotationAxis == EVRInteractibleLeverAxis::Axis_XY ? PxD6Motion::eLIMITED : PxD6Motion::eLOCKED);
					NewJoint->setMotion(PxD6Axis::eSWING1, LeverRotationAxis == EVRInteractibleLeverAxis::Axis_Y || LeverRotationAxis == EVRInteractibleLeverAxis::Axis_XY ? PxD6Motion::eLIMITED : PxD6Motion::eLOCKED);
					NewJoint->setMotion(PxD6Axis::eSWING2, PxD6Motion::eLOCKED);

					const float CorrectedLeverLimit = (LeverLimitPositive + LeverLimitNegative) / 2;
					const float LeverLimitRad = CorrectedLeverLimit * (PI / 180.0f);
					//PxReal LimitContactDistance = FMath::DegreesToRadians(FMath::Max(1.f, ProfileInstance.ConeLimit.ContactDistance));

					//The limit values need to be clamped so it will be valid in PhysX
					PxReal ZLimitAngle = FMath::ClampAngle(CorrectedLeverLimit, KINDA_SMALL_NUMBER, 179.9999f) * (PI / 180.0f);
					PxReal YLimitAngle = FMath::ClampAngle(CorrectedLeverLimit, KINDA_SMALL_NUMBER, 179.9999f) * (PI / 180.0f);
					//PxReal LimitContactDistance = FMath::DegreesToRadians(FMath::Max(1.f, ProfileInstance.ConeLimit.ContactDistance * FMath::Min(InSwing1LimitScale, InSwing2LimitScale)));

					NewJoint->setSwingLimit(PxJointLimitCone(YLimitAngle, ZLimitAngle));
					NewJoint->setTwistLimit(PxJointAngularLimitPair(-LeverLimitRad, LeverLimitRad));

					return true;
				}
			}
			return false;
		});
	}

#else
	return false;
#endif // WITH_PHYSX

	return false;
}

float UVRLeverComponent::ReCalculateCurrentAngle(bool bAllowThrowingEvents)
{
	FTransform CurRelativeTransform = this->GetComponentTransform().GetRelativeTransform(UVRInteractibleFunctionLibrary::Interactible_GetCurrentParentTransform(this));
	CalculateCurrentAngle(CurRelativeTransform);
	ProccessCurrentState(bIsLerping, bAllowThrowingEvents, bAllowThrowingEvents);
	return CurrentLeverAngle;
}

void UVRLeverComponent::SetLeverAngle(float NewAngle, FVector DualAxisForwardVector, bool bAllowThrowingEvents)
{
	NewAngle = -NewAngle; // Need to inverse the sign

	FVector ForwardVector = DualAxisForwardVector;
	switch (LeverRotationAxis)
	{
	case EVRInteractibleLeverAxis::Axis_X:
		ForwardVector = FVector(FMath::Sign(NewAngle), 0.0f, 0.0f); break;
	case EVRInteractibleLeverAxis::Axis_Y:
		ForwardVector = FVector(0.0f, FMath::Sign(NewAngle), 0.0f); break;
	case EVRInteractibleLeverAxis::Axis_Z:
		ForwardVector = FVector(0.0f, 0.0f, FMath::Sign(NewAngle)); break;
	default:break;
	}

	FQuat NewLeverRotation(ForwardVector, FMath::DegreesToRadians(FMath::Abs(NewAngle)));

	this->SetRelativeTransform(FTransform(NewLeverRotation) * InitialRelativeTransform);
	ReCalculateCurrentAngle(bAllowThrowingEvents);
}

void UVRLeverComponent::ResetInitialLeverLocation(bool bAllowThrowingEvents)
{
	// Get our initial relative transform to our parent (or not if un-parented).
	InitialRelativeTransform = this->GetRelativeTransform();
	CalculateCurrentAngle(InitialRelativeTransform);
	ProccessCurrentState(bIsLerping, bAllowThrowingEvents, bAllowThrowingEvents);
}

void UVRLeverComponent::CalculateCurrentAngle(FTransform & CurrentTransform)
{
	float Angle;
	switch (LeverRotationAxis)
	{
	case EVRInteractibleLeverAxis::Axis_XY:
	case EVRInteractibleLeverAxis::FlightStick_XY:
	{
		FTransform RelativeToSpace = CurrentTransform.GetRelativeTransform(InitialRelativeTransform);
		FQuat CurrentRelRot = RelativeToSpace.GetRotation();// CurrentTransform.GetRotation();
		FVector UpVec = CurrentRelRot.GetUpVector();

		CurrentLeverForwardVector = FVector::VectorPlaneProject(UpVec, FVector::UpVector);
		CurrentLeverForwardVector.Normalize();

		FullCurrentAngle = FMath::RadiansToDegrees(FMath::Acos(FVector::DotProduct(UpVec, FVector::UpVector)));
		CurrentLeverAngle = FMath::RoundToFloat(FullCurrentAngle);

		AllCurrentLeverAngles.Roll = FMath::Sign(UpVec.Y) * FMath::RadiansToDegrees(FMath::Acos(FVector::DotProduct(FVector(0.0f, UpVec.Y, UpVec.Z), FVector::UpVector)));
		AllCurrentLeverAngles.Pitch = FMath::Sign(UpVec.X) * FMath::RadiansToDegrees(FMath::Acos(FVector::DotProduct(FVector(UpVec.X, 0.0f, UpVec.Z), FVector::UpVector)));

		if (bBlendAxisValuesByAngleThreshold)
		{
			FVector ProjectedLoc = FVector(UpVec.X, UpVec.Y, 0.0f).GetSafeNormal();
			AllCurrentLeverAngles.Pitch *= FMath::Clamp(1.0f - (FMath::Abs(FMath::RadiansToDegrees(FMath::Acos(FVector::DotProduct(ProjectedLoc, FMath::Sign(UpVec.X) * FVector::ForwardVector)))) / AngleThreshold), 0.0f, 1.0f);
			AllCurrentLeverAngles.Roll *= FMath::Clamp(1.0f - (FMath::Abs(FMath::RadiansToDegrees(FMath::Acos(FVector::DotProduct(ProjectedLoc, FMath::Sign(UpVec.Y) * FVector::RightVector)))) / AngleThreshold), 0.0f, 1.0f);
		}

		AllCurrentLeverAngles.Roll = FMath::RoundToFloat(AllCurrentLeverAngles.Roll);
		AllCurrentLeverAngles.Pitch = FMath::RoundToFloat(AllCurrentLeverAngles.Pitch);

		if (LeverRotationAxis == EVRInteractibleLeverAxis::FlightStick_XY)
		{
			AllCurrentLeverAngles.Yaw = FMath::RoundToFloat(UVRExpansionFunctionLibrary::GetHMDPureYaw_I(CurrentRelRot.Rotator()).Yaw);
		}
		else
			AllCurrentLeverAngles.Yaw = 0.0f;

	}break;
	default:
	{
		Angle = UVRInteractibleFunctionLibrary::GetDeltaAngleFromTransforms((EVRInteractibleAxis)LeverRotationAxis, InitialRelativeTransform, CurrentTransform);
		FullCurrentAngle = Angle;
		CurrentLeverAngle = FMath::RoundToFloat(FullCurrentAngle);
		CurrentLeverForwardVector = UVRInteractibleFunctionLibrary::SetAxisValueVec((EVRInteractibleAxis)LeverRotationAxis, FMath::Sign(Angle));
		AllCurrentLeverAngles = UVRInteractibleFunctionLibrary::SetAxisValueRot((EVRInteractibleAxis)LeverRotationAxis, CurrentLeverAngle, FRotator::ZeroRotator);

	}break;
	}
}

void UVRLeverComponent::LerpAxis(float CurrentAngle, float DeltaTime)
{
	float TargetAngle = 0.0f;
	float FinalReturnSpeed = LeverReturnSpeed;

	switch (LeverReturnTypeWhenReleased)
	{
	case EVRInteractibleLeverReturnType::LerpToMax:
	{
		if (CurrentAngle >= 0)
			TargetAngle = FMath::RoundToFloat(LeverLimitPositive);
		else
			TargetAngle = -FMath::RoundToFloat(LeverLimitNegative);
	}break;
	case EVRInteractibleLeverReturnType::LerpToMaxIfOverThreshold:
	{
		if ((!FMath::IsNearlyZero(LeverLimitPositive) && CurrentAngle >= (LeverLimitPositive * LeverTogglePercentage)))
			TargetAngle = FMath::RoundToFloat(LeverLimitPositive);
		else if ((!FMath::IsNearlyZero(LeverLimitNegative) && CurrentAngle <= -(LeverLimitNegative * LeverTogglePercentage)))
			TargetAngle = -FMath::RoundToFloat(LeverLimitNegative);
	}break;
	case EVRInteractibleLeverReturnType::RetainMomentum:
	{
		if (FMath::IsNearlyZero(MomentumAtDrop * DeltaTime, 0.1f))
		{
			MomentumAtDrop = 0.0f;
			this->SetComponentTickEnabled(false);
			bIsLerping = false;
			bReplicateMovement = bOriginalReplicatesMovement;
			return;
		}
		else
		{
			MomentumAtDrop = FMath::FInterpTo(MomentumAtDrop, 0.0f, DeltaTime, LeverMomentumFriction);

			FinalReturnSpeed = FMath::Abs(MomentumAtDrop);

			if (MomentumAtDrop >= 0.0f)
				TargetAngle = FMath::RoundToFloat(LeverLimitPositive);
			else
				TargetAngle = -FMath::RoundToFloat(LeverLimitNegative);
		}

	}break;
	case EVRInteractibleLeverReturnType::ReturnToZero:
	default:
	{}break;
	}

	//float LerpedVal = FMath::FixedTurn(CurrentAngle, TargetAngle, FinalReturnSpeed * DeltaTime);
	float LerpedVal = FMath::FInterpConstantTo(CurrentAngle, TargetAngle, DeltaTime, FinalReturnSpeed);

	if (FMath::IsNearlyEqual(LerpedVal, TargetAngle))
	{
		if (LeverRestitution > 0.0f)
		{
			MomentumAtDrop = -(MomentumAtDrop * LeverRestitution);
			FTransform CalcTransform = (FTransform(UVRInteractibleFunctionLibrary::SetAxisValueRot((EVRInteractibleAxis)LeverRotationAxis, TargetAngle, FRotator::ZeroRotator)) * InitialRelativeTransform);
			this->SetRelativeRotation(CalcTransform.Rotator());
		}
		else
		{
			this->SetComponentTickEnabled(false);
			bIsLerping = false;
			bReplicateMovement = bOriginalReplicatesMovement;
			FTransform CalcTransform = (FTransform(UVRInteractibleFunctionLibrary::SetAxisValueRot((EVRInteractibleAxis)LeverRotationAxis, TargetAngle, FRotator::ZeroRotator)) * InitialRelativeTransform);
			this->SetRelativeRotation(CalcTransform.Rotator());
		}
	}
	else
	{
		FTransform CalcTransform = (FTransform(UVRInteractibleFunctionLibrary::SetAxisValueRot((EVRInteractibleAxis)LeverRotationAxis, LerpedVal, FRotator::ZeroRotator)) * InitialRelativeTransform);
		this->SetRelativeRotation(CalcTransform.Rotator());
	}
}

float UVRLeverComponent::CalcAngle(EVRInteractibleLeverAxis AxisToCalc, FVector CurInteractorLocation, bool bSkipLimits)
{
	float ReturnAxis = 0.0f;

	ReturnAxis = UVRInteractibleFunctionLibrary::GetAtan2Angle((EVRInteractibleAxis)AxisToCalc, CurInteractorLocation, InitialGripRot);

	if (bSkipLimits)
		return ReturnAxis;

	if (LeverLimitPositive > 0.0f && LeverLimitNegative > 0.0f && FMath::IsNearlyEqual(LeverLimitNegative, 180.f, 0.01f) && FMath::IsNearlyEqual(LeverLimitPositive, 180.f, 0.01f))
	{
		// Don't run the clamping or the flip detection, we are a 360 degree lever
	}
	else
	{
		ReturnAxis = FMath::ClampAngle(FRotator::NormalizeAxis(RotAtGrab + ReturnAxis), -LeverLimitNegative, LeverLimitPositive);

		// Ignore rotations that would flip the angle of the lever to the other side, with a 90 degree allowance
		if (!bIsInFirstTick && ((LeverLimitPositive > 0.0f && LastDeltaAngle >= LeverLimitPositive) || (LeverLimitNegative > 0.0f && LastDeltaAngle <= -LeverLimitNegative)) && FMath::Sign(LastDeltaAngle) != FMath::Sign(ReturnAxis))
		{
			ReturnAxis = LastDeltaAngle;
		}
	}

	bIsInFirstTick = false;
	return ReturnAxis;
}