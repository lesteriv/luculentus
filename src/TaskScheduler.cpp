// Luculentus -- Proof of concept spectral path tracer
// Copyright (C) 2012-2014  Ruud van Asseldonk
// 
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
// 
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
// 
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

#include "TaskScheduler.h"

#include <iostream>
#include <numeric>

using namespace Luculentus;
using std::chrono::steady_clock;
using std::chrono::duration_cast;

const steady_clock::duration TaskScheduler::tonemappingInterval = std::chrono::seconds(30);

TaskScheduler::TaskScheduler(const int numberOfThreads, const int width,
                             const int height, const Scene& scene)
{
  // More trace units than threads seems sensible,
  // but less plot units is acceptable,
  // because one plot unit can handle multiple trace units
  numberOfTraceUnits = std::max(1, numberOfThreads * 3);
  numberOfPlotUnits  = std::max(1, numberOfThreads / 2);

  // Allocate some space for the work unit arrays
  traceUnits.reserve(numberOfTraceUnits);
  plotUnits.reserve(numberOfPlotUnits);

  // Build all the trace units, with a different random seed for all units
  unsigned long randomSeed = std::random_device()();
  for (size_t i = 0; i < numberOfTraceUnits; i++)
  {
    traceUnits.emplace_back(scene, randomSeed, width, height);
    // Pick a different random seed for the next trace unit
    randomSeed = traceUnits[i].monteCarloUnit.randomEngine();
  }

  // Then build the plot units
  for (size_t i = 0; i < numberOfPlotUnits; i++)
  {
    plotUnits.emplace_back(width, height);
  }

  // There must be one gather unit
  gatherUnit = std::unique_ptr<GatherUnit>(new GatherUnit(width, height));

  // And finally the tonemap unit
  tonemapUnit = std::unique_ptr<TonemapUnit>(new TonemapUnit(width, height));

  // Everything is available at this point
  for (int i = 0; i < (int)numberOfTraceUnits; i++) availableTraceUnits.push(i);
  for (int i = 0; i < (int)numberOfPlotUnits; i++) availablePlotUnits.push(i);
  gatherUnitAvailable = true;
  tonemapUnitAvailable = true;

  // The image has not changed (there is none)
  imageChanged = false;

  // Tonemap as soon as possible
  lastTonemapTime = steady_clock::now();
  completedTraces = 0;
}

Task TaskScheduler::GetNewTask(const Task completedTask)
{
  // Acuire a lock during this function;
  // no threads may simultaneously access the scheduling functionality
  std::unique_lock<std::mutex> lock(mutex);

  // Make units that were used by the completed task available again
  CompleteTask(completedTask);

  // If the last tonemapping time was more than x seconds ago,
  // an update should be done
  auto now = steady_clock::now();
  if (now - lastTonemapTime > tonemappingInterval)
  {
    // If the image has changed since it was last tonemapped,
    // tonemap it now
    if (imageChanged)
    {
      // Tonemapping can only be done if no gathering
      // and tonemapping units are busy
      if (gatherUnitAvailable && tonemapUnitAvailable)
        return CreateTonemapTask();
    }
    else
    {
      // Otherwise, plots must first be gathered, tonemapping will
      // happen once that is done
      if (gatherUnitAvailable && !donePlotUnits.empty())
        return CreateGatherTask();
    }
  }

  // If a substantial number of trace units is done, plot them first
  // so they can be recycled soon
  if (doneTraceUnits.size() > numberOfTraceUnits / 2
      && !availablePlotUnits.empty()) return CreatePlotTask();

  // Then, if there are enough trace units available, go trace some rays!
  if (!availableTraceUnits.empty())
  {
    return CreateTraceTask();
  }

  // Otherwise, some trace units need to be plotted to make them
  // available again
  if (!availablePlotUnits.empty() && !doneTraceUnits.empty())
  {
    return CreatePlotTask();
  }

  // If no plot units are available (or all trace units are busy,
  // which should be impossible), gather some plots to make the plot
  // units available again
  if (gatherUnitAvailable && !donePlotUnits.empty())
  {
    return CreateGatherTask();
  }

  // If everything is locked in dependencies and everything is a big
  // mess, simply wait a while for units to become available
  return CreateSleepTask();
}

Task TaskScheduler::CreateSleepTask()
{
  Task task; task.type = Task::Sleep;
  return task;
}

Task TaskScheduler::CreateTraceTask()
{
  // Pick the first available trace unit, and use it for the task
  Task task; task.type = Task::Trace;
  task.unit = availableTraceUnits.front();
  availableTraceUnits.pop();

  return task;
}

Task TaskScheduler::CreatePlotTask()
{
  // Pick the first available plot unit, and use it for the task
  Task task; task.type = Task::Plot;
  task.unit = availablePlotUnits.front();
  availablePlotUnits.pop();

  // Take around half of the trace units which are done for this task
  const size_t done = doneTraceUnits.size();
  const size_t n = std::min(done, std::max<size_t>(1, done / 2));

  // Have it plot trace units which are done
  for (size_t i = 0; i < n; i++)
  {
    task.otherUnits.push_back(doneTraceUnits.front());
    doneTraceUnits.pop();
  }

  return task;
}

Task TaskScheduler::CreateGatherTask()
{
  Task task; task.type = Task::Gather;

  // The gather unit will be busy gathering
  gatherUnitAvailable = false;

  // Have it gather all plot units which are done
  while (!donePlotUnits.empty())
  {
    task.otherUnits.push_back(donePlotUnits.front());
    donePlotUnits.pop();
  }

  return task;
}

Task TaskScheduler::CreateTonemapTask()
{
  Task task; task.type = Task::Tonemap;

  // The gather unit must not be used during tonemapping,
  // because the tonemap unit is reading from it
  gatherUnitAvailable = false;
  tonemapUnitAvailable = false;

  return task;
}

void TaskScheduler::CompleteTask(const Task completedTask)
{
  // Delegate the task to the correct method
  switch (completedTask.type)
  {
    case Task::Trace:   CompleteTraceTask(completedTask);   break;
    case Task::Plot:    CompletePlotTask(completedTask);    break;
    case Task::Gather:  CompleteGatherTask(completedTask);  break;
    case Task::Tonemap: CompleteTonemapTask();              break;
    case Task::Sleep:                                       break;
  }

  // The 'Sleep' task is ignored; it consumes no resources
  if (completedTask.type == Task::Sleep) std::cout << ".";
}

void TaskScheduler::CompleteTraceTask(const Task completedTask)
{
  std::cout << "done tracing with unit " << completedTask.unit << std::endl;

  // The trace unit used for the task, now need plotting before it is
  // available again
  doneTraceUnits.push(completedTask.unit);

  completedTraces++;
}

void TaskScheduler::CompletePlotTask(Task completedTask)
{
  std::cout << "done plotting with unit " << completedTask.unit << std::endl;
  std::cout << "the following trace units are available again: ";

  // All the trace units that were plotted, can be used again now
  while (!completedTask.otherUnits.empty())
  {
    availableTraceUnits.push(completedTask.otherUnits.back());
    completedTask.otherUnits.pop_back();
    std::cout << " " << availableTraceUnits.back() << " ";
  }

  std::cout << std::endl;

  // And the plot unit that was used, needs to be gathered before it can
  // be used again
  donePlotUnits.push(completedTask.unit);
}

void TaskScheduler::CompleteGatherTask(Task completedTask)
{
  std::cout << "done gathering" << std::endl;
  std::cout << "the following plot units are available again: ";

  // All the plot units that were gathered, can be used again now
  while (!completedTask.otherUnits.empty())
  {
    availablePlotUnits.push(completedTask.otherUnits.back());
    completedTask.otherUnits.pop_back();
    std::cout << " " << availablePlotUnits.back() << " ";
  }

  std::cout << std::endl;

  // And the gather unit can now be used again as well
  gatherUnitAvailable = true;

  // The image must have changed because of gathering
  imageChanged = true;
}

#include <algorithm>

void TaskScheduler::CompleteTonemapTask()
{
  std::cout << "done tonemapping" << std::endl;

  // The tonemapper needed the gather unit, so the gather unit is free now
  gatherUnitAvailable = true;

  // And of course the tonemap unit itself is available again
  tonemapUnitAvailable = true;

  // The image is tonemapped now, so until a new gathering happens,
  // it will not change
  imageChanged = false;

  // Measure how many rays per second the renderer can handle.
  const auto now = steady_clock::now();
  const auto renderTime = now - lastTonemapTime;
  const auto ms = duration_cast<std::chrono::milliseconds>(renderTime);
  const auto batchesPerSecond = completedTraces * 1000.0f / ms.count();
  lastTonemapTime = now;
  completedTraces = 0;

  // Store the latest 512 measurements (should be about 4.25 hours).
  performance.push_back(batchesPerSecond);
  if (performance.size() > 512) performance.pop_front();
  float n = static_cast<float>(performance.size());

  float mean = std::accumulate(performance.begin(), performance.end(), 0.0f) / n;

  float sqrMean = std::accumulate(performance.begin(), performance.end(), 0.0f,
                  [](float a, float perf) { return a + perf * perf; }) / n;

  float variance = sqrMean - mean * mean;

  std::cout << "performance: " << mean << " +- " << std::sqrt(variance)
            << " batches/sec" << std::endl;
}
