// Fill out your copyright notice in the Description page of Project Settings.

#include "GeneticTriangles.h"
#include "PathManager.h"

#include "Path.h"

// Sets default values
APathManager::APathManager() :
	SceneComponent(nullptr),
	PopulationCount(0),
	MaxInitialVariation(40.0f),
	MinAmountOfPointsPerPathAtStartup(5),
	MaxAmountOfPointsPerPathAtStartup(5),
	TimeBetweenGenerations(1.0f),
	CrossoverProbability(70.0f),
	mTimer(TimeBetweenGenerations),
	AverageFitness(0.0f),
	AmountOfNodesWeight(100.0f),
	ProximityToTargetedNodeWeight(100.0f),
	LengthWeight(100.0f),
	CanSeeTargetWeight(100.0f),
	ObstacleHitMultiplier(1.0f),
	AggregateSelectOne(false),
	MutationProbability(5.0f),
	TranslatePointProbability(33.333f),
	InsertionProbability(33.333f),
	DeletionProbability(33.333f)
{
 	// Set this actor to call Tick() every frame.  You can turn this off to improve performance if you don't need it.
	PrimaryActorTick.bCanEverTick = true;

	// Exposes the scene component so we may actually move the actor in the scene
	SceneComponent = CreateDefaultSubobject<USceneComponent>(TEXT("SceneComponent"));
	RootComponent = SceneComponent;
}

// Called when the game starts or when spawned
void APathManager::BeginPlay()
{
	Super::BeginPlay();
	
	// Create population
	mPaths.Empty();
	mPaths.Reserve(PopulationCount);

	for (int32 i = 0; i < PopulationCount; ++i)
	{
		APath* path = GetWorld()->SpawnActor<APath>(GetTransform().GetLocation(), GetTransform().GetRotation().Rotator());

		ensure(path != nullptr);

		path->PostInit(MinAmountOfPointsPerPathAtStartup, MaxAmountOfPointsPerPathAtStartup);
		path->RandomizeValues(Nodes[0], MaxInitialVariation);
		path->DetermineGeneticRepresentation();

		mPaths.Add(path);
	}
}

// Called every frame
void APathManager::Tick( float DeltaTime )
{
	Super::Tick( DeltaTime );

	// Start a countdown so we run a generation each interval
	mTimer -= DeltaTime;
	if (mTimer < 0.0f)
	{
		mTimer = TimeBetweenGenerations;

		RunGeneration();
	}
}


// Allow dispose handling before destructing
void APathManager::Dispose()
{
	this->Destroy();
}



void APathManager::RunGeneration()
{
	if (Nodes.IsValidIndex(0) && Nodes.IsValidIndex(1) && Nodes[0]->IsValidLowLevelFast() && Nodes[1]->IsValidLowLevelFast())
	{
		EvaluateFitness();
		SelectionStep();
		CrossoverStep();
		MutationStep();
		EvaluateFitness();
		ColorCodePathsByFitness();

		mGenerationInfo.mGenerationNumber = GenerationCount++;

		LogGenerationInfo();
	}
	else
		UE_LOG(LogTemp, Warning, TEXT("APathManager::RunGeneration() >> One of the nodes is invalid!"));
}


void APathManager::EvaluateFitness()
{
	/*if (GEngine != nullptr)
		GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Yellow, TEXT("Starting fitness evaluation..."));
		*/
	// What defines fitness for a path?
	// 1. SHORTEST / CLOSEST
	// -> Amount of chunks per path (less chunks == more fitness)
	// -> Length of a path (shorter l => higher f)
	// -> Distance of the final node in relation to the targeted node
	// -> Average orientation of the path

	// Fitness is calculated as an agreation of multiple fitness values

	// /////////////////////////
	// 1. DATA AND STATE CACHING
	// /////////////////////////
	// Determine the least and most amount of nodes as this will influence the way fitness is calculated as well
	int32 least_amount_of_nodes = INT32_MAX;
	int32 most_amount_of_nodes = 0;

	// Same goes for the distances between the final point of a path and the targetted node
	float closest_distance = TNumericLimits<float>::Max();
	float furthest_distance = 0.0f;
	FVector targetting_location = Nodes[1]->GetActorLocation(); // Assumes this has been filled in through the editor

	// And again, same goes for the length of the path
	float shortest_path_length = TNumericLimits<float>::Max();
	float longest_path_length = 0.0f;

	for (int32 i = 0; i < mPaths.Num(); ++i)
	{
		APath* path = nullptr;

		if (mPaths.IsValidIndex(i) && mPaths[i]->IsValidLowLevelFast())
		{
			path = mPaths[i];

			if (path == nullptr && GEngine != nullptr)
			{
				GEngine->AddOnScreenDebugMessage(-1, 10.0f, FColor::White, TEXT("Nullptr path in fitness evaluation!"));
				continue;
			}

			// Force path to snap to terrain if possible
			path->SnapToTerrain();

			// Node amount calculation
			const int32 node_amount = path->GetAmountOfNodes();

			if (node_amount < least_amount_of_nodes)
				least_amount_of_nodes = node_amount;

			if (node_amount > most_amount_of_nodes)
				most_amount_of_nodes = node_amount;

			// Distance calculations
			const float distance_to_targetting_node = (targetting_location - path->GetLocationOfFinalNode()).Size();

			if (distance_to_targetting_node < closest_distance)
				closest_distance = distance_to_targetting_node;

			if (distance_to_targetting_node > furthest_distance)
				furthest_distance = distance_to_targetting_node;

			// Length calculation
			const float path_length = path->GetLength();
			
			if (path_length < shortest_path_length)
				shortest_path_length = path_length;

			if (path_length > longest_path_length)
				longest_path_length = path_length;

			// Trace & slope handling
			const TArray<FVector>& genetic_representation = path->GetGeneticRepresentation();
			for (int32 index = 1; index < genetic_representation.Num(); ++index)
			{
				if (GetWorld() != nullptr && genetic_representation.IsValidIndex(index) && genetic_representation.IsValidIndex(index - 1))
				{
					// Check for obstacles
					FHitResult obstacle_hit_result;
					if (GetWorld()->LineTraceSingleByChannel(obstacle_hit_result, genetic_representation[index - 1], genetic_representation[index], ECollisionChannel::ECC_GameTraceChannel1))
						path->MarkIsInObstacle();

					// Check for terrain traveling (hidden)
					FHitResult terrain_hit_result;
					if (GetWorld()->LineTraceSingleByChannel(terrain_hit_result, genetic_representation[index - 1], genetic_representation[index], ECollisionChannel::ECC_GameTraceChannel4))
						path->MarkTravelingThroughTerrain();
				}

				// Check if the head is able to see the target node
				if (GetWorld() != nullptr && genetic_representation.IsValidIndex(index) && index == genetic_representation.Num() - 1)
				{
					FHitResult hit_result;
					if (!GetWorld()->LineTraceSingleByChannel(hit_result, genetic_representation[index], Nodes.IsValidIndex(1) ? Nodes[1]->GetActorLocation() : FVector::ZeroVector, ECollisionChannel::ECC_GameTraceChannel2))
						path->MarkCanSeeTarget();
				}

				// Check if the slope between this node and the previous is inbetween the expected bounds
				// Use dot product calculation between the vector between the two points and a vector with a constant Z
				if (genetic_representation.IsValidIndex(i) && genetic_representation.IsValidIndex(i - 1))
				{
					FVector direction = genetic_representation[i] - genetic_representation[i - 1];
					FVector collapsed_vector = direction;
					collapsed_vector.Z = 0.0f;

					direction.Normalize();
					collapsed_vector.Normalize();
					
					const float dot_product = FVector::DotProduct(direction, collapsed_vector);
					const float radians = FMath::Acos(dot_product);
					const float degrees = FMath::RadiansToDegrees(radians);

					// Check if radians are radians and degrees are degrees, as Acos doesn't specify what the return value is
					// Check Kismet
					// UE_LOG(LogTemp, Warning, TEXT("Radians: %f Degrees: %f"), radians, degrees);

					if (degrees > MaxSlopeToleranceAngle)
						path->MarkSlopeTooIntense();
				}
			}

			// Check if the head is inside the node sphere
			if ((Nodes[1]->GetActorLocation() - genetic_representation.Last()).Size() < 100.0f) // @TODO: Magic value
				path->MarkHasReachedTarget();
		}
		else
			UE_LOG(LogTemp, Warning, TEXT("APathManager::EvaluateFitness >> mPaths contains an invalid APath* at index %d"), i);
	}
	
	// TODO: Currently should ALWAYS result in 5


	// ///////////////////////////////
	// 2. CALCULATE AND ASSIGN FITNESS
	// ///////////////////////////////
	mTotalFitness = 0.0f;
	int32 amount_of_nodes = 0;
	for (int32 i = 0; i < mPaths.Num(); ++i)
	{
		APath* path = nullptr;

		if (mPaths.IsValidIndex(i) && mPaths[i]->IsValidLowLevelFast())
		{
			path = mPaths[i];

			// Final node in relation to targetting node
			// Length
			// Amount of nodes

			// Blend value
			// Y = (X- X0) / (X1 - X0)

			// Need zero handling
			float node_amount_blend_value = 0.0f;
			if (least_amount_of_nodes - most_amount_of_nodes != 0)
			{
				node_amount_blend_value = (path->GetAmountOfNodes() - most_amount_of_nodes) / (least_amount_of_nodes - most_amount_of_nodes);
			}

			float proximity_blend_value = 0.0f;
			if (FMath::Abs(closest_distance - furthest_distance) > 0.1f)
			{
				proximity_blend_value = ((targetting_location - path->GetLocationOfFinalNode()).Size() - furthest_distance) / (closest_distance - furthest_distance);
			}

			float length_blend_value = 0.0f;
			if (FMath::Abs(shortest_path_length - longest_path_length) > 0.1f)
			{
				length_blend_value = (path->GetLength() - longest_path_length) / (shortest_path_length - longest_path_length);
			}

			// Determine if the path is able to see the target node
			float can_see_target_fitness = 0.0f;
			if (path->GetCanSeeTarget())
				can_see_target_fitness = CanSeeTargetWeight;
			
			// Path has reached target, mark fit
			float target_reached_fitness = 0.0f;
			if (path->GetHasReachedTarget())
				target_reached_fitness = TargetReachedWeight;

			// Should the path hit an obstacle, mark it unfit
			float obstacle_multiplier = 1.0f;
			if (path->GetIsInObstacle())
				obstacle_multiplier = ObstacleHitMultiplier;

			// Slope too intense for the path to continue on, mark unfit
			float slope_too_intense_multiplier = 1.0f;
			if (path->GetSlopeTooIntense())
				slope_too_intense_multiplier = SlopeTooIntenseMultiplier;

			// Path traveling through terrain?
			float traveling_through_terrain_multiplier = 1.0f;
			if (path->GetTravelingThroughTerrain())
				traveling_through_terrain_multiplier = PiercesTerrainMultiplier;

			// Calculate final fitness based on the various weights and multipliers
			const float final_fitness = ((AmountOfNodesWeight * node_amount_blend_value) + 
										(ProximityToTargetedNodeWeight * proximity_blend_value) +
										(LengthWeight * length_blend_value) +
										can_see_target_fitness +
										target_reached_fitness +
										SlopeWeight) *
										obstacle_multiplier * 
										slope_too_intense_multiplier *
										traveling_through_terrain_multiplier;

			path->SetFitness(final_fitness);
			
			mTotalFitness += final_fitness;
			amount_of_nodes += path->GetGeneticRepresentation().Num();
		}
		else
			UE_LOG(LogTemp, Warning, TEXT("APathManager::EvaluateFitness >> mPaths contains an invalid APath* at index %d"), i);
	}

	AverageFitness = mTotalFitness / mPaths.Num();
	
	mGenerationInfo.mAverageFitness = AverageFitness;
	mGenerationInfo.mAverageAmountOfNodes = amount_of_nodes / (float)mPaths.Num();
	
	const float max_fitness = AmountOfNodesWeight + ProximityToTargetedNodeWeight + LengthWeight + CanSeeTargetWeight + TargetReachedWeight + SlopeWeight;
	mGenerationInfo.mMaximumFitness = max_fitness;
	mGenerationInfo.mFitnessFactor = AverageFitness / max_fitness;
		
	// ////////////////////////////////////
	// 3. SORT PATHS BY FITNESS, DESCENDING
	// ////////////////////////////////////
	mPaths.Sort([&](const APath& lhs, const APath& rhs) 
	{
		return lhs.GetFitness() > rhs.GetFitness();
	});
}



void APathManager::SelectionStep()
{
	mMatingPaths.Empty();
	mMatingPaths.Reserve(PopulationCount);

	// Still roulette wheel sampling
	while (mMatingPaths.Num() < PopulationCount)
	{
		const float R = FMath::FRand();
		float accumulated_fitness = 0.0f;

		for (int32 i = 0; i < mPaths.Num(); ++i)
		{
			APath* path = nullptr;

			if (mPaths.IsValidIndex(i) && mPaths[i]->IsValidLowLevelFast())
			{
				path = mPaths[i];

				accumulated_fitness += path->GetFitness() / mTotalFitness;

				if (accumulated_fitness >= R)
				{
					mMatingPaths.Add(path);
					break;
				}
			}
			else
				UE_LOG(LogTemp, Warning, TEXT("APathManager::SelectionStep >> mPaths contains an invalid APath* at index %d"), i);
		}
	}

}



void APathManager::CrossoverStep()
{
	TArray<APath*> temp;
	temp.Reserve(PopulationCount);

	int32 successfull_crossover_amount = 0;

	for (int32 i = 0; i < mMatingPaths.Num(); i += 2)
	{
		const float R = FMath::FRandRange(0.0f, 100.0f);

		if (R >= (100.0f - CrossoverProbability))
		{
			const APath* current_path = mMatingPaths[i];
			const APath* next_path = mMatingPaths[i + 1];
			const APath* smallest_path = current_path;
			const APath* bigger_path = next_path;

			// Compare the two paths based on their node amount
			// Operator < might be better for this
			if (current_path->GetAmountOfNodes() > next_path->GetAmountOfNodes())
			{
				smallest_path = next_path;
				bigger_path = current_path;
			}

			// Code below can be put in a switch
			if (CrossoverOperator == ECrossoverOperator::SinglePoint)
			{
				const int crossover_point = FMath::RandRange(0, smallest_path->GetAmountOfNodes());
				APath* offspring_0 = GetWorld()->SpawnActor<APath>(GetTransform().GetLocation(), GetTransform().GetRotation().Rotator());;
				APath* offspring_1 = GetWorld()->SpawnActor<APath>(GetTransform().GetLocation(), GetTransform().GetRotation().Rotator());;

				int32 index = 0;
				for (const FVector& ref : bigger_path->GetGeneticRepresentation())
				{
					// Junk data evaluation
					if (index >= smallest_path->GetAmountOfNodes())
					{
						const float appending_chance = FMath::RandRange(0.0f, 100.0f);

						if (appending_chance < JunkDNACrossoverProbability)
							offspring_0->AddChromosome(bigger_path->GetChromosome(index));

						const float appending_chance_next = FMath::RandRange(0.0f, 100.0f);
						if (appending_chance_next < JunkDNACrossoverProbability)
							offspring_1->AddChromosome(bigger_path->GetChromosome(index));
					}
					else
					{
						if (index < crossover_point)
						{
							offspring_0->AddChromosome(smallest_path->GetChromosome(index));
							offspring_1->AddChromosome(bigger_path->GetChromosome(index));
						}
						else
						{
							offspring_0->AddChromosome(bigger_path->GetChromosome(index));
							offspring_1->AddChromosome(smallest_path->GetChromosome(index));
						}
					}

					++index;
				}

				offspring_0->DetermineGeneticRepresentation();
				offspring_1->DetermineGeneticRepresentation();

				temp.Add(offspring_0);
				temp.Add(offspring_1);

				++successfull_crossover_amount;
			}
			else if (CrossoverOperator == ECrossoverOperator::Uniform)
			{
				APath* offspring_0 = GetWorld()->SpawnActor<APath>(GetTransform().GetLocation(), GetTransform().GetRotation().Rotator());;
				APath* offspring_1 = GetWorld()->SpawnActor<APath>(GetTransform().GetLocation(), GetTransform().GetRotation().Rotator());;

				int32 index = 0;
				for (const FVector& ref : bigger_path->GetGeneticRepresentation())
				{
					if (index >= smallest_path->GetAmountOfNodes())
					{
						// Evaluate junk data
						// Both offsrping have a shot of copying the junk data of the less fit parent
						const float junk_chance = FMath::FRandRange(0.0f, 100.0f);
						if (junk_chance < JunkDNACrossoverProbability)
							offspring_0->AddChromosome(ref);

						const float junk_chance_next = FMath::FRandRange(0.0f, 100.0f);
						if (junk_chance_next < JunkDNACrossoverProbability)
							offspring_1->AddChromosome(ref);
					}
					else
					{
						// Uniform does crossover per chromosome
						const float bias = FMath::FRandRange(0.0f, 100.0f);
						if (bias < 50.0f)
						{
							offspring_0->AddChromosome(smallest_path->GetChromosome(index));
							offspring_1->AddChromosome(bigger_path->GetChromosome(index));
						}
						else
						{
							offspring_0->AddChromosome(bigger_path->GetChromosome(index));
							offspring_1->AddChromosome(smallest_path->GetChromosome(index));
						}
					}

					++index;
				}

				offspring_0->DetermineGeneticRepresentation();
				offspring_1->DetermineGeneticRepresentation();

				temp.Add(offspring_0);
				temp.Add(offspring_1);

				++successfull_crossover_amount;
			}
		}
		else
		{
			// Unable to crossover
			// Parents are duplicated to the next generation
			APath* duplicate_0 = GetWorld()->SpawnActor<APath>(GetTransform().GetLocation(), GetTransform().GetRotation().Rotator());
			duplicate_0->SetGeneticRepresentation(mMatingPaths[i]->GetGeneticRepresentation());
			duplicate_0->DetermineGeneticRepresentation();
			temp.Add(duplicate_0);

			APath* duplicate_1 = GetWorld()->SpawnActor<APath>(GetTransform().GetLocation(), GetTransform().GetRotation().Rotator());
			duplicate_1->SetGeneticRepresentation(mMatingPaths[i+1]->GetGeneticRepresentation());
			duplicate_1->DetermineGeneticRepresentation();
			temp.Add(duplicate_1);
		}
	}

	Purge();

	mPaths = temp;

	mGenerationInfo.mCrossoverAmount = successfull_crossover_amount;

	/*if (GEngine != nullptr)
		GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Red, TEXT("Amount of crossovers: ") +FString::FromInt(successfull_crossover_amount));*/
}


void APathManager::MutationStep()
{
	// Mutate a node
	// Mutate multiple nodes
	// Insert a node
	// Delete a node

	/*if (GEngine != nullptr)
		GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Orange, TEXT("Mutation step..."));
		*/
	int32 successful_translation_mutations = 0;
	int32 successful_insertion_mutations = 0;
	int32 successful_deletion_mutations = 0;

	for (APath* path : mPaths)
	{
		// Every path may be considered for mutation
		const float rand = FMath::FRandRange(0.0f, 100.0f);
		if (rand < MutationProbability)
		{
			// Determine which mutation occurs
			//EMutationType mutation_type{};

			bool do_translation_mutation = false;
			bool do_insertion_mutation = false;
			bool do_deletion_mutation = false;

			if (AggregateSelectOne)
			{
				const float aggregated_probability = TranslatePointProbability + InsertionProbability + DeletionProbability;
				// @TODO:
			}
			else
			{
				const float translate_point_probability = FMath::FRandRange(0, 100.0f);
				if (translate_point_probability < TranslatePointProbability)
					do_translation_mutation = true;

				const float insert_point_probability = FMath::FRandRange(0, 100.0f);
				if (insert_point_probability < InsertionProbability)
					do_insertion_mutation = true;

				const float deletion_probability = FMath::FRandRange(0, 100.0f);
				if (deletion_probability < DeletionProbability)
					do_deletion_mutation = true;
			}

			// Do mutation in Path
			if (do_translation_mutation)
			{
				path->MutateThroughTranslation(TranslationMutationType, MaxTranslationOffset);
				++successful_translation_mutations;
			}	
			if (do_insertion_mutation)
			{
				path->MutateThroughInsertion();
				++successful_insertion_mutations;
			}
			if (do_deletion_mutation)
			{
				path->MutateThroughDeletion();
				++successful_deletion_mutations;
			}
		}
	}

	mGenerationInfo.mAmountOfTranslationMutations = successful_translation_mutations;
	mGenerationInfo.mAmountOfInsertionMutations = successful_insertion_mutations;
	mGenerationInfo.mAmountOfDeletionMutations = successful_deletion_mutations;
}



void APathManager::Purge()
{
	for (int32 i = mMatingPaths.Num() - 1; i > -1; --i)
	{
		if (mMatingPaths.IsValidIndex(i) && mMatingPaths[i]->IsValidLowLevelFast())
			mMatingPaths[i]->Destroy();

		if (mPaths.IsValidIndex(i) && mPaths[i]->IsValidLowLevelFast())
			mPaths[i]->Destroy();
	}
}



void APathManager::ColorCodePathsByFitness()
{
	float lowest_fitness = TNumericLimits<float>::Max();
	float highest_fitness = 0.0f;

	// Cache lowest & highest fitness first
	// @TODO: Do this in the loop of EvaluateFitness()
	for (const APath* path : mPaths)
	{
		const float fitness = path->GetFitness();

		if (fitness < lowest_fitness)
			lowest_fitness = fitness;
		if (fitness > highest_fitness)
			highest_fitness = fitness;
	}

	for (APath* path : mPaths)
	{
		check(path != nullptr);

		if (path->GetIsInObstacle() || path->GetSlopeTooIntense() || path->GetTravelingThroughTerrain()) 
		{
			// Completely unfit paths are marked grey
			path->SetColorCode(InvalidPathColor);
		}
		else
		{
			// Apply color coding based on the lowest and highest fitness values
			const float fitness = path->GetFitness();
			const float blend_value = (fitness - highest_fitness) / (lowest_fitness - highest_fitness);

			FColor red = FColor::Red;
			FColor green = FColor::Green;

			FColor blended;
			blended.A = 255;
			blended.R = FMath::Lerp(red.R, green.R, blend_value * 255);
			blended.G = FMath::Lerp(red.G, green.G, blend_value * 255);
			blended.B = FMath::Lerp(red.B, green.B, blend_value * 255);

			path->SetColorCode(blended);
		}
	}
}


void APathManager::LogGenerationInfo()
{
	if (GEngine != nullptr)
	{
		GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Black, TEXT("\n\n"));

		GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Green, TEXT("Average amount of nodes: ") + FString::SanitizeFloat(mGenerationInfo.mAverageAmountOfNodes));
		GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::White, TEXT("Fitness factor: ") + FString::SanitizeFloat(mGenerationInfo.mFitnessFactor));
		GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Cyan, TEXT("Maximum fitness: ") + FString::SanitizeFloat(mGenerationInfo.mMaximumFitness));
		GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::White, TEXT("Average fitness: ") + FString::SanitizeFloat(mGenerationInfo.mAverageFitness));
		GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Orange, TEXT("Amount of deletion mutations: ") + FString::FromInt(mGenerationInfo.mAmountOfDeletionMutations));
		GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Yellow, TEXT("Amount of insertion mutations: ") + FString::FromInt(mGenerationInfo.mAmountOfInsertionMutations));
		GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Orange, TEXT("Amount of translation mutations: ") + FString::FromInt(mGenerationInfo.mAmountOfTranslationMutations));
		GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Red, TEXT("Amount of reproducing crossovers: ") + FString::FromInt(mGenerationInfo.mCrossoverAmount));
		GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Green, TEXT("Generation #") + FString::FromInt(mGenerationInfo.mGenerationNumber));
	}
}