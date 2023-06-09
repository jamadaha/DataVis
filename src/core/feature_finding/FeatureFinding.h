#ifndef FEATUREEXTRACTION_FEATUREFINDING_H
#define FEATUREEXTRACTION_FEATUREFINDING_H

#include <memory>
#include <stdexcept>
#include <random>
#include <algorithm>
#include <iterator>
#include <iostream>
#include <vector>
#include "misc/Constants.h"
#include "utilities/SeriesUtils.h"
#include "core/InformationGain.h"
#include "core/WindowGeneration.h"
#include "IO/Logger.h"
#include "types/Feature.h"
#include "core/attributes/Attribute.h"
#include <thread>
#include <future>
#include "Evaluation.h"

#include "misc/DataManager.h"

namespace FeatureFinding {
    static void FindOptimalFeature(const std::vector<LabelledSeries> &series,
                                   const ClassCount &counts,
                                   double entropy,
                                   const std::vector<Series> &windows,
                                   uint startIndex,
                                   uint endIndex,
                                   std::shared_ptr<Feature> &optimalFeature,
                                   std::mutex &featureMutex,
                                   const std::vector<std::shared_ptr<Attribute>> &attributes) {
        double optimalGain = 0;
        for (uint i = startIndex; i < endIndex; i++) {
            const auto& window = windows.at(i);
            for (const auto &attribute: attributes) {
                const double gain = Evaluation::EvaluateWindow(entropy, optimalGain, counts, attribute, series, window);

                if (gain > optimalGain) {
                    while (!featureMutex.try_lock()){}
                    optimalGain = ((optimalFeature != nullptr) ? optimalFeature->gain : 0);
                    if (optimalFeature == nullptr || gain > optimalGain)
                        optimalFeature = std::make_shared<Feature>(Feature(window, attribute, gain));
                    featureMutex.unlock();
                }
            }
        }
    }

    /// Multi-threaded. Evaluates each window in \p windows, in combination with each attribute
    /// \return Returns the feature with highest information gain. Can be nullptr if no valid feature found.
    [[nodiscard]] std::shared_ptr<Feature> FindOptimalFeature(const std::vector<LabelledSeries> &series, const std::vector<Series> &windows,
                                                              const std::vector<std::shared_ptr<Attribute>> &attributes) {
        if (series.size() < 2)
            throw std::logic_error("Cannot find features for less than two series.");
        if (windows.empty())
            throw std::logic_error("Missing windows.");

        const uint threadCount = std::thread::hardware_concurrency();

        const ClassCount count = SeriesUtils::GetCount(series);
        const double entropy = InformationGain::CalculateEntropy(count);

        std::thread threads[maxThreads];
        std::shared_ptr<Feature> optimalFeature;
        std::mutex featureMutex;

        for (uint i = 0; i < threadCount; i++) {
            const uint startIndex = i * (windows.size() / threadCount);
            const uint endIndex = (i + 1) * (windows.size() / threadCount);
            threads[i] = std::thread([&, i] { FindOptimalFeature(series, count, entropy, windows, startIndex, endIndex, optimalFeature, featureMutex, attributes); } );
        }

        for (uint i = 0; i < threadCount; i++)
            threads[i].join();

        return optimalFeature;
    }

    /// Generates a number of features based upon samples from each class
    /// \param seriesMap Data to generate features from, where the key is class and value is series set
    /// \param featureCount How many features to generate
    /// \param sampleSize How many samples to take from each class for each feature. If \p sampleSize is 3, it takes
    /// a 3 samples from each class.
    /// \return A list of features. Can be empty if no valid features are found.
    void GenerateFeaturesFromSamples(const std::unordered_map<int, std::vector<Series>> &seriesMap,
                                                                   uint minWindowSize, uint maxWindowSize,
                                                                   uint minSampleSize, uint maxSampleSize,
                                                                   uint featureCount, std::vector<std::shared_ptr<Attribute>> attributes) {
        uint attempts = 0;
        DataManager::BeginFeatureGeneration();

        while (DataManager::FeatureCount() < featureCount) {
            if (attempts > 1000)
                break;
            std::vector<LabelledSeries> samples;
            uint diffClassCount = 0; // How many classes are included in the feature

            // Retrieve n samples from each class
            for (const auto &seriesSet : seriesMap) {
                std::vector<Series> tempSamples;
                const uint sampleSize = (rand() % ((maxSampleSize == 0) ? seriesSet.second.size() : maxSampleSize)) + minSampleSize;
                std::sample(seriesSet.second.begin(), seriesSet.second.end(), std::back_inserter(tempSamples), sampleSize, rd);
                for (const auto &sample : tempSamples)
                    samples.emplace_back(seriesSet.first, sample);
                if (!tempSamples.empty())
                    diffClassCount++;
            }
            if (diffClassCount < 2) {
                attempts++;
                continue;
            }

            // Generate feature based on samples
            auto windows = WindowGeneration::GenerateWindowsOfMinMaxLength(samples, minWindowSize, maxWindowSize);
            WindowGeneration::RemoveDuplicateWindows(&windows);
            auto feature = FindOptimalFeature(samples, windows, attributes);
            if (feature != nullptr) {
                auto tempFeatures = DataManager::GetFeatures();
                if (std::find(tempFeatures.begin(), tempFeatures.end(), *feature) != tempFeatures.end()) {
                    attempts++;
                    continue;
                }
                DataManager::AddFeature(*feature);
            }
            attempts = 0;
        }
        DataManager::EndFeatureGeneration();
    }
}

#endif //FEATUREEXTRACTION_FEATUREFINDING_H
