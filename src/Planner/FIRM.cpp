/*********************************************************************
* Software License Agreement (BSD License)
*
*  Copyright (c) 2014, Texas A&M University
*  All rights reserved.
*
*  Redistribution and use in source and binary forms, with or without
*  modification, are permitted provided that the following conditions
*  are met:
*
*   * Redistributions of source code must retain the above copyright
*     notice, this list of conditions and the following disclaimer.
*   * Redistributions in binary form must reproduce the above
*     copyright notice, this list of conditions and the following
*     disclaimer in the documentation and/or other materials provided
*     with the distribution.
*   * Neither the name of the Rice University nor the names of its
*     contributors may be used to endorse or promote products derived
*     from this software without specific prior written permission.
*
*  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
*  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
*  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
*  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
*  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
*  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
*  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
*  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
*  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
*  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
*  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
*  POSSIBILITY OF SUCH DAMAGE.
*********************************************************************/
/* Author: Saurav Agarwal, Ali-akbar Agha-mohammadi */

#include "../../include/Planner/FIRM.h"
#include "ompl/geometric/planners/prm/PRM.h"
#include "ompl/geometric/planners/prm/ConnectionStrategy.h"
#include "ompl/base/goals/GoalSampleableRegion.h"
#include "ompl/base/objectives/PathLengthOptimizationObjective.h"
#include "ompl/datastructures/PDF.h"
#include "ompl/tools/config/SelfConfig.h"
#include "ompl/tools/config/MagicConstants.h"
#include <boost/lambda/bind.hpp>
#include <boost/graph/astar_search.hpp>
#include <boost/graph/incremental_components.hpp>
#include <boost/property_map/vector_property_map.hpp>
#include <boost/foreach.hpp>
#include <boost/thread.hpp>

#include "GoalVisitor.hpp"

#define foreach BOOST_FOREACH
#define foreach_reverse BOOST_REVERSE_FOREACH

namespace ompl
{
    namespace magic
    {

        /** \brief The number of steps to take for a random bounce
            motion generated as part of the expansion step of PRM. */
        static const unsigned int MAX_RANDOM_BOUNCE_STEPS   = 2;

        /** \brief The number of nearest neighbors to consider by
            default in the construction of the PRM roadmap */
        static const unsigned int DEFAULT_NEAREST_NEIGHBORS = 10;

        /** \brief The time in seconds for a single roadmap building operation (dt)*/
        static const double ROADMAP_BUILD_TIME = 0.5;

        static const double NUM_MONTE_CARLO_PARTICLES = 2;

        static const double EXTREMELY_HIGH_EDGE_COST = 1e6;

        static const double NON_OBSERVABLE_NODE_COVARIANCE = 1e2;

        static const float DYNAMIC_PROGRAMMING_DISCOUNT_FACTOR = 1;

        static const double GOAL_COST_TO_GO = 0;

        static const double INIT_COST_TO_GO = 0;

        static const double OBSTACLE_COST_TO_GO = 500;

        static const double DP_CONVERGENCE_THRESHOLD = 1e-3;
    }
}

FIRM::FIRM(const firm::SpaceInformation::SpaceInformationPtr &si, bool starStrategy) :
    ompl::base::Planner(si, "FIRM"),
    siF_(si),
    starStrategy_(starStrategy),
    stateProperty_(boost::get(vertex_state_t(), g_)),
    totalConnectionAttemptsProperty_(boost::get(vertex_total_connection_attempts_t(), g_)),
    successfulConnectionAttemptsProperty_(boost::get(vertex_successful_connection_attempts_t(), g_)),
    weightProperty_(boost::get(boost::edge_weight, g_)),
    edgeIDProperty_(boost::get(boost::edge_index, g_)),
    disjointSets_(boost::get(boost::vertex_rank, g_),
                  boost::get(boost::vertex_predecessor, g_)),
    maxEdgeID_(0),
    userSetConnectionStrategy_(false),
    addedSolution_(false)
{
    specs_.recognizedGoal = ompl::base::GOAL_SAMPLEABLE_REGION;
    specs_.approximateSolutions = true;
    specs_.optimizingPaths = true;

    Planner::declareParam<unsigned int>("max_nearest_neighbors", this, &FIRM::setMaxNearestNeighbors, std::string("8:1000"));
    debug_ = true;
}

FIRM::~FIRM(void)
{
    freeMemory();
}

void FIRM::setup(void)
{
    Planner::setup();
    if (!nn_)
    {
        nn_.reset(ompl::tools::SelfConfig::getDefaultNearestNeighbors<Vertex>(si_->getStateSpace()));
        nn_->setDistanceFunction(boost::bind(&FIRM::distanceFunction, this, _1, _2));
    }
    if (!connectionStrategy_)
    {
        if (starStrategy_)
            connectionStrategy_ = ompl::geometric::KStarStrategy<Vertex>(boost::bind(&FIRM::milestoneCount, this), nn_, si_->getStateDimension());
        else
            connectionStrategy_ = ompl::geometric::KStrategy<Vertex>(ompl::magic::DEFAULT_NEAREST_NEIGHBORS, nn_);
    }
    if (!connectionFilter_)
        connectionFilter_ = boost::lambda::constant(true);

    if (pdef_->hasOptimizationObjective())
        opt_ = pdef_->getOptimizationObjective();
    else
    {
        opt_.reset(new ompl::base::PathLengthOptimizationObjective(si_));
        opt_->setCostThreshold(opt_->infiniteCost());
    }

    int np = ompl::magic::NUM_MONTE_CARLO_PARTICLES;
    numParticles_ = np;

}

void FIRM::setMaxNearestNeighbors(unsigned int k)
{
    if (!nn_)
    {
        nn_.reset(ompl::tools::SelfConfig::getDefaultNearestNeighbors<Vertex>(si_->getStateSpace()));
        nn_->setDistanceFunction(boost::bind(&FIRM::distanceFunction, this, _1, _2));
    }
    connectionStrategy_ = ompl::geometric::KStrategy<Vertex>(k, nn_);
}

void FIRM::setProblemDefinition(const ompl::base::ProblemDefinitionPtr &pdef)
{
    Planner::setProblemDefinition(pdef);
    clearQuery();
}

void FIRM::clearQuery(void)
{
    startM_.clear();
    goalM_.clear();
    pis_.restart();
}

void FIRM::clear(void)
{
    Planner::clear();
    sampler_.reset();
    simpleSampler_.reset();
    freeMemory();
    if (nn_)
        nn_->clear();
    clearQuery();
    maxEdgeID_ = 0;
}

void FIRM::freeMemory(void)
{
    foreach (Vertex v, boost::vertices(g_))
        si_->freeState(stateProperty_[v]);
    g_.clear();
}

void FIRM::expandRoadmap(double expandTime)
{
    expandRoadmap(ompl::base::timedPlannerTerminationCondition(expandTime));
}

void FIRM::expandRoadmap(const ompl::base::PlannerTerminationCondition &ptc)
{
    if (!simpleSampler_)
        simpleSampler_ = si_->allocStateSampler();

    std::vector<ompl::base::State*> states(ompl::magic::MAX_RANDOM_BOUNCE_STEPS);
    si_->allocStates(states);
    expandRoadmap(ptc, states);
    si_->freeStates(states);
}

void FIRM::expandRoadmap(const ompl::base::PlannerTerminationCondition &ptc,
                                         std::vector<ompl::base::State*> &workStates)
{
    // construct a probability distribution over the vertices in the roadmap
    // as indicated in
    //  "Probabilistic Roadmaps for Path Planning in High-Dimensional Configuration Spaces"
    //        Lydia E. Kavraki, Petr Svestka, Jean-Claude Latombe, and Mark H. Overmars

    ompl::PDF<Vertex> pdf;
    foreach (Vertex v, boost::vertices(g_))
    {
        const unsigned int t = totalConnectionAttemptsProperty_[v];
        pdf.add(v, (double)(t - successfulConnectionAttemptsProperty_[v]) / (double)t);
    }

    if (pdf.empty())
        return;

    while (ptc == false)
    {
        Vertex v = pdf.sample(rng_.uniform01());
        unsigned int s = si_->randomBounceMotion(simpleSampler_, stateProperty_[v], workStates.size(), workStates, false);
        if (s > 0)
        {
            s--;
            Vertex last = addMilestone(si_->cloneState(workStates[s]));

            graphMutex_.lock();
            for (unsigned int i = 0 ; i < s ; ++i)
            {
                // add the vertex along the bouncing motion
                Vertex m = boost::add_vertex(g_);
                stateProperty_[m] = si_->cloneState(workStates[i]);
                totalConnectionAttemptsProperty_[m] = 1;
                successfulConnectionAttemptsProperty_[m] = 0;
                disjointSets_.make_set(m);

                // add the edge to the parent vertex
                //const ompl::base::Cost weight = opt_->motionCost(stateProperty_[v], stateProperty_[m]);
                EdgeControllerType edgeController;
                NodeControllerType nodeController;
                const FIRMWeight weight = generateControllersWithEdgeCost(stateProperty_[v], stateProperty_[m], edgeController, nodeController);
                const unsigned int id = maxEdgeID_++;
                const Graph::edge_property_type properties(weight, id);
                std::pair<Edge, bool> newEdge = boost::add_edge(v, m, properties, g_);
                edgeControllers_[newEdge.first] = edgeController;
                nodeControllers_[m] = nodeController;
                //transitionProbabilities_[newEdge.first] = 1;
                uniteComponents(v, m);

                // add the vertex to the nearest neighbors data structure
                nn_->add(m);
                v = m;
            }

            // if there are intermediary states or the milestone has not been connected to the initially sampled vertex,
            // we add an edge
            if (s > 0 || !sameComponent(v, last))
            {
                // add the edge to the parent vertex
                //const ompl::base::Cost weight = opt_->motionCost(stateProperty_[v], stateProperty_[last]);
                EdgeControllerType edgeController;
                NodeControllerType nodeController;
                const FIRMWeight weight = generateControllersWithEdgeCost(stateProperty_[v], stateProperty_[last], edgeController, nodeController);
                const unsigned int id = maxEdgeID_++;
                const Graph::edge_property_type properties(weight, id);
                std::pair<Edge, bool> newEdge = boost::add_edge(v, last, properties, g_);
                edgeControllers_[newEdge.first] = edgeController;
                nodeControllers_[last] = nodeController; // should this be last or v ???
                transitionProbabilities_[newEdge.first] = 1;
                uniteComponents(v, last);
            }
            graphMutex_.unlock();
        }
    }
}

void FIRM::growRoadmap(double growTime)
{
    growRoadmap(ompl::base::timedPlannerTerminationCondition(growTime));
}

void FIRM::growRoadmap(const ompl::base::PlannerTerminationCondition &ptc)
{
    if (!isSetup())
        setup();
    if (!sampler_)
        sampler_ = si_->allocValidStateSampler();

    ompl::base::State *workState = si_->allocState();
    growRoadmap (ptc, workState);
    si_->freeState(workState);
}

void FIRM::growRoadmap(const ompl::base::PlannerTerminationCondition &ptc,
                                       ompl::base::State *workState)
{
    /* grow roadmap in the regular fashion -- sample valid states, add them to the roadmap, add valid connections */
    while (ptc == false)
    {
        // search for a valid state
        bool found = false;
        while (!found && ptc == false)
        {
            unsigned int attempts = 0;
            do
            {
                found = sampler_->sample(workState);
                attempts++;
            } while (attempts < ompl::magic::FIND_VALID_STATE_ATTEMPTS_WITHOUT_TERMINATION_CHECK && !found);
        }
        // add it as a milestone
        if (found)
            addMilestone(si_->cloneState(workState));
    }
}

void FIRM::checkForSolution(const ompl::base::PlannerTerminationCondition &ptc,
                                            ompl::base::PathPtr &solution)
{
    ompl::base::GoalSampleableRegion *goal = static_cast<ompl::base::GoalSampleableRegion*>(pdef_->getGoal().get());
    while (!ptc && !addedSolution_)
    {
        // Check for any new goal states
        if (goal->maxSampleCount() > goalM_.size())
        {
            const ompl::base::State *st = pis_.nextGoal();
            if (st)
                goalM_.push_back(addMilestone(si_->cloneState(st)));
        }

        // Check for a solution
        addedSolution_ = haveSolution(startM_, goalM_, solution);
        // Sleep for 1ms
        if (!addedSolution_)
            boost::this_thread::sleep(boost::posix_time::milliseconds(1));
    }
}

bool FIRM::haveSolution(const std::vector<Vertex> &starts, const std::vector<Vertex> &goals, ompl::base::PathPtr &solution)
{
    ompl::base::Goal *g = pdef_->getGoal().get();
    ompl::base::Cost sol_cost(0.0);
    bool sol_cost_set = false;
    foreach (Vertex start, starts)
    {
        foreach (Vertex goal, goals)
        {
            // we lock because the connected components algorithm is incremental and may change disjointSets_
            graphMutex_.lock();
            bool same_component = sameComponent(start, goal);
            graphMutex_.unlock();

            if (same_component && g->isStartGoalPairValid(stateProperty_[goal], stateProperty_[start]))
            {
                //ompl::base::PathPtr p = constructSolution(start, goal);
                boost::mutex::scoped_lock _(graphMutex_);
                solveDynamicProgram(goal);

                return true;
                /*
                if (p)
                {
                    // Check if optimization objective is satisfied
                    ompl::base::Cost pathCost = p->cost(opt_);
                    if (opt_->isSatisfied(pathCost))
                    {
                        solution = p;
                        return true;
                    }
                    else if (!sol_cost_set || opt_->isCostBetterThan(pathCost, sol_cost))
                    {
                        solution = p;
                        sol_cost = pathCost;
                        sol_cost_set = true;
                    }
                }
                */
            }
        }
    }

    return false;
}

bool FIRM::addedNewSolution(void) const
{
    return addedSolution_;
}

ompl::base::PlannerStatus FIRM::solve(const ompl::base::PlannerTerminationCondition &ptc)
{
    checkValidity();
    ompl::base::GoalSampleableRegion *goal = dynamic_cast<ompl::base::GoalSampleableRegion*>(pdef_->getGoal().get());

    if (!goal)
    {
        OMPL_ERROR("%s: Unknown type of goal", getName().c_str());
        return ompl::base::PlannerStatus::UNRECOGNIZED_GOAL_TYPE;
    }

    // Add the valid start states as milestones
    while (const ompl::base::State *st = pis_.nextStart())
        startM_.push_back(addMilestone(si_->cloneState(st)));

    if (startM_.size() == 0)
    {
        OMPL_ERROR("%s: There are no valid initial states!", getName().c_str());
        return ompl::base::PlannerStatus::INVALID_START;
    }

    if (!goal->couldSample())
    {
        OMPL_ERROR("%s: Insufficient states in sampleable goal region", getName().c_str());
        return ompl::base::PlannerStatus::INVALID_GOAL;
    }

    // Ensure there is at least one valid goal state
    if (goal->maxSampleCount() > goalM_.size() || goalM_.empty())
    {
        const ompl::base::State *st = goalM_.empty() ? pis_.nextGoal(ptc) : pis_.nextGoal();
        if (st)
            goalM_.push_back(addMilestone(si_->cloneState(st)));

        if (goalM_.empty())
        {
            OMPL_ERROR("%s: Unable to find any valid goal states", getName().c_str());
            return ompl::base::PlannerStatus::INVALID_GOAL;
        }
    }

    unsigned int nrStartStates = boost::num_vertices(g_);
    OMPL_INFORM("%s: Starting with %u states", getName().c_str(), nrStartStates);

    // Reset addedSolution_ member and create solution checking thread
    addedSolution_ = false;
    ompl::base::PathPtr sol;
    boost::thread slnThread(boost::bind(&FIRM::checkForSolution, this, ptc, boost::ref(sol)));

    // construct new planner termination condition that fires when the given ptc is true, or a solution is found
    ompl::base::PlannerTerminationCondition ptcOrSolutionFound =
        ompl::base::plannerOrTerminationCondition(ptc, ompl::base::PlannerTerminationCondition(boost::bind(&FIRM::addedNewSolution, this)));

    constructRoadmap(ptcOrSolutionFound);

    // Ensure slnThread is ceased before exiting solve
    slnThread.join();

    OMPL_INFORM("%s: Created %u states", getName().c_str(), boost::num_vertices(g_) - nrStartStates);

    //---- JUST FOR TESTING---
    executeFeedback();
    //-------------------

    if (sol)
    {
        ompl::base::PlannerSolution psol(sol);
        // if the solution was optimized, we mark it as such
        if (addedNewSolution())
            psol.optimized_ = true;
        pdef_->addSolutionPath (psol);
    }

    // Return true if any solution was found.
    return sol ? (addedNewSolution() ? ompl::base::PlannerStatus::EXACT_SOLUTION : ompl::base::PlannerStatus::APPROXIMATE_SOLUTION) : ompl::base::PlannerStatus::TIMEOUT;
}

void FIRM::constructRoadmap(const ompl::base::PlannerTerminationCondition &ptc)
{
    if (!isSetup())
        setup();
    if (!sampler_)
        sampler_ = si_->allocValidStateSampler();
    if (!simpleSampler_)
        simpleSampler_ = si_->allocStateSampler();

    std::vector<ompl::base::State*> xstates(ompl::magic::MAX_RANDOM_BOUNCE_STEPS);
    si_->allocStates(xstates);
    bool grow = true;

    while (ptc() == false)
    {
        // maintain a 2:1 ratio for growing/expansion of roadmap
        // call growRoadmap() twice as long for every call of expandRoadmap()
        if (grow)
            growRoadmap(ompl::base::plannerOrTerminationCondition(ptc, ompl::base::timedPlannerTerminationCondition(2.0 * ompl::magic::ROADMAP_BUILD_TIME)), xstates[0]);
        else
            expandRoadmap(ompl::base::plannerOrTerminationCondition(ptc, ompl::base::timedPlannerTerminationCondition(ompl::magic::ROADMAP_BUILD_TIME)), xstates);
        grow = !grow;
    }

    si_->freeStates(xstates);
}

FIRM::Vertex FIRM::addMilestone(ompl::base::State *state)
{
    boost::mutex::scoped_lock _(graphMutex_);

    Vertex m = boost::add_vertex(g_);
    //std::cout<<"Putting out the vertex property :"<<m<<std::endl;
    //std::cin.get();
    stateProperty_[m] = state;
    totalConnectionAttemptsProperty_[m] = 1;
    successfulConnectionAttemptsProperty_[m] = 0;

    // Initialize to its own (dis)connected component.
    disjointSets_.make_set(m);

    nn_->add(m);

    // Which milestones will we attempt to connect to?
    const std::vector<Vertex>& neighbors = connectionStrategy_(m);

    foreach (Vertex n, neighbors)
        if (connectionFilter_(m, n))
        {
            totalConnectionAttemptsProperty_[m]++;
            totalConnectionAttemptsProperty_[n]++;
            if (si_->checkMotion(stateProperty_[m], stateProperty_[n]))
            {
                successfulConnectionAttemptsProperty_[m]++;
                successfulConnectionAttemptsProperty_[n]++;
                // This where we perform MC simulation to get edge cost
                //const ompl::base::Cost weight = opt_->motionCost(stateProperty_[m], stateProperty_[n]);
                EdgeControllerType edgeController;
                NodeControllerType nodeController;
                const FIRMWeight weight = generateControllersWithEdgeCost(stateProperty_[m], stateProperty_[n], edgeController, nodeController);
                const unsigned int id = maxEdgeID_++;
                const Graph::edge_property_type properties(weight, id);
                std::pair<Edge, bool> newEdge = boost::add_edge(m, n, properties, g_);
                edgeControllers_[newEdge.first] = edgeController;
                nodeControllers_[m] = nodeController;
                transitionProbabilities_[newEdge.first] = 1; // TODO: these probabilities should come from Monte Carlo
                uniteComponents(n, m);

            }
        }

    return m;
}

void FIRM::uniteComponents(Vertex m1, Vertex m2)
{
    disjointSets_.union_set(m1, m2);
}

bool FIRM::sameComponent(Vertex m1, Vertex m2)
{
    return boost::same_component(m1, m2, disjointSets_);
}


ompl::base::PathPtr FIRM::constructSolution(const Vertex &start, const Vertex &goal)
{
    boost::mutex::scoped_lock _(graphMutex_);
    boost::vector_property_map<Vertex> prev(boost::num_vertices(g_));
    /*
    try
    {
        // Consider using a persistent distance_map if it's slow

        boost::astar_search(g_, start,
                            boost::bind(&FIRM::costHeuristic, this, _1, goal),
                            boost::predecessor_map(prev).
                            distance_compare(boost::bind(&ompl::base::OptimizationObjective::
                                                         isCostBetterThan, opt_.get(), _1, _2)).
                            distance_combine(boost::bind(&ompl::base::OptimizationObjective::
                                                         combineCosts, opt_.get(), _1, _2)).
                            distance_inf(opt_->infiniteCost()).
                            distance_zero(opt_->identityCost()).
                            visitor(AStarGoalVisitor<Vertex>(goal)));


    }
    catch (AStarFoundGoal&)
    {
    }
    */
    if (prev[goal] == goal)
        throw ompl::Exception(name_, "Could not find solution path");
    else
        return constructGeometricPath(prev, start, goal);
}

ompl::base::PathPtr FIRM::constructGeometricPath(const boost::vector_property_map<Vertex> &prev, const Vertex &start, const Vertex &goal)
{
    ompl::geometric::PathGeometric *p = new ompl::geometric::PathGeometric(si_);
    for (Vertex pos = goal; prev[pos] != pos; pos = prev[pos])
        p->append(stateProperty_[pos]);
    p->append(stateProperty_[start]);
    p->reverse();

    return ompl::base::PathPtr(p);
}

void FIRM::getPlannerData(ompl::base::PlannerData &data) const
{
    Planner::getPlannerData(data);

    // Explicitly add start and goal states:
    for (size_t i = 0; i < startM_.size(); ++i)
        data.addStartVertex(ompl::base::PlannerDataVertex(stateProperty_[startM_[i]], const_cast<FIRM*>(this)->disjointSets_.find_set(startM_[i])));

    for (size_t i = 0; i < goalM_.size(); ++i)
        data.addGoalVertex(ompl::base::PlannerDataVertex(stateProperty_[goalM_[i]], const_cast<FIRM*>(this)->disjointSets_.find_set(goalM_[i])));

    // Adding edges and all other vertices simultaneously
    foreach(const Edge e, boost::edges(g_))
    {
        const Vertex v1 = boost::source(e, g_);
        const Vertex v2 = boost::target(e, g_);
        data.addEdge(ompl::base::PlannerDataVertex(stateProperty_[v1]),
                     ompl::base::PlannerDataVertex(stateProperty_[v2]));

        // Add the reverse edge, since we're constructing an undirected roadmap
        data.addEdge(ompl::base::PlannerDataVertex(stateProperty_[v2]),
                     ompl::base::PlannerDataVertex(stateProperty_[v1]));

        // Add tags for the newly added vertices
        data.tagState(stateProperty_[v1], const_cast<FIRM*>(this)->disjointSets_.find_set(v1));
        data.tagState(stateProperty_[v2], const_cast<FIRM*>(this)->disjointSets_.find_set(v2));
    }
}

ompl::base::Cost FIRM::costHeuristic(Vertex u, Vertex v) const
{
    return opt_->motionCostHeuristic(stateProperty_[u], stateProperty_[v]);
}


FIRMWeight FIRM::generateControllersWithEdgeCost(ompl::base::State* startNodeState, ompl::base::State* targetNodeState, EdgeControllerType &edgeController, NodeControllerType &nodeController)
{

    if(debug_)
    {
        std::cout<<"The 2 states to connect for edge are: "<<std::endl;
        std::cout<<"Start  : \n"<<std::endl;
        std::cout<<startNodeState->as<SE2BeliefSpace::StateType>()->getArmaData();
        std::cout<<"End  : \n"<<std::endl;
        std::cout<<targetNodeState->as<SE2BeliefSpace::StateType>()->getArmaData();
        //std::cout<<"Press Enter and wait "<<std::endl;
        //std::cin.get();
    }
    double successCount = 0;

    // initialize costs to 0
    ompl::base::Cost edgeCost(0);
    ompl::base::Cost nodeStabilizationCost(0);

    // Generate the edge controller for given start and end state
    generateEdgeController(startNodeState,targetNodeState,edgeController);

    // Generate the node controller
    generateNodeController(targetNodeState, nodeController);

    for(int i=0; i< numParticles_;i++)
    {

        bool isCollided = false;
        //cout << "MonteCarlo Simulation particle number "<< i<<endl;
        double tempWeight=0;

        siF_->setTrueState(startNodeState);
        siF_->setBelief(targetNodeState);

        if(debug_)
        {
            //cout << "initial belief for the edge: "<<endl<< _c1.GetArmaData()[0]<<endl<<_c1.GetArmaData()[1]<<endl<<_c1.GetArmaData()[2]*180/PI<< endl;
            //cout << "goal belief for the edge: "<<endl<< _c2.GetArmaData()[0]<<endl<<_c2.GetArmaData()[1]<<endl<<_c2.GetArmaData()[2]*180/PI<< endl;
            //cout<<"The goal node covariance is: "<<_c2.m_covariance<<endl;
            //cin.get();
        }
        ompl::base::State* endBelief = siF_->allocState(); // allocate the end state of the controller
        ompl::base::Cost pcost(0);

        if(edgeController.Execute(startNodeState, endBelief, pcost))
        {
           successCount++;

           edgeCost.v = edgeCost.v + pcost.v ;

            if(debug_)
            {
                //std::cout<<"The cost at particle: "<<i<<"  is : "<<edgeCost.v<<std::endl;
                //std::cin.get();
            }
        }

    }

    if (successCount > 0)  edgeCost.v = edgeCost.v / successCount ;
    else edgeCost.v = ompl::magic::EXTREMELY_HIGH_EDGE_COST; // extremely high cost if no particle could succeed, we can also simply not add this edge

    double transitionProbability = successCount / numParticles_ ;

    if(debug_)
    {
        std::cout<<"Edge Cost :"<<edgeCost.v<<std::endl;
        std::cout<<"Transition Prob: "<<transitionProbability<<std::endl;
        //std::cout<<"Press Enter"<<std::endl;
        //std::cin.get();
    }

    FIRMWeight weight(edgeCost.v, transitionProbability);

    return weight;
}

void FIRM::generateEdgeController(const ompl::base::State *start, const ompl::base::State* target, FIRM::EdgeControllerType &edgeController)
{
    std::vector<ompl::base::State*> intermediates;

    ompl::base::State *intermediate = si_->allocState();

    si_->copyState(intermediate, start);

    std::vector<ompl::control::Control*> openLoopControls;

    // get the open loop controls for this edge
    siF_->getMotionModel()->generateOpenLoopControls(start, target, openLoopControls);

    // generate the intermediate states using the open loop controls
    for(typename std::vector<ompl::control::Control*>::iterator c=openLoopControls.begin(), e=openLoopControls.end(); c!=e; ++c)
    {
        ompl::base::State *x = si_->allocState();
        siF_->getMotionModel()->Evolve(intermediate,*c,siF_->getMotionModel()->getZeroNoise(), x);
        intermediates.push_back(x);
        si_->copyState(intermediate, x);
    }

    // create the edge controller
    EdgeControllerType ctrlr(target, intermediates, openLoopControls, siF_);

    // assign the edge controller
    edgeController =  ctrlr;
}

void FIRM::generateNodeController(const ompl::base::State *state, FIRM::NodeControllerType &nodeController)
{
    // Create a copy of the node state
    ompl::base::State *node = si_->allocState();
    siF_->copyState(node, state);

   if(siF_->getObservationModel()->isStateObservable(node))
   {
        if(debug_) std::cout<<"The node is observable, constructing node controller \n"<<std::endl;
        // Contruct a linear kalman filter
        LinearizedKF linearizedKF(siF_);

        //Construct a linear system
        LinearSystem linearSystem(node, siF_->getMotionModel()->getZeroControl(),siF_->getObservationModel()->getObservation(state, false), siF_->getMotionModel(), siF_->getObservationModel());

        // Compute the stationary cov at node state using LKF
        arma::mat stationaryCovariance = linearizedKF.computeStationaryCovariance(linearSystem);

        // set the covariance
        node->as<SE2BeliefSpace::StateType>()->setCovariance(stationaryCovariance);

        // create a node controller
        std::vector<ompl::control::Control*> dummyControl;
        std::vector<ompl::base::State*> dummyStates;
        NodeControllerType ctrlr(node, dummyStates, dummyControl, siF_);

        // assign the node controller
        nodeController = ctrlr;
    }
    else
    {
        if(debug_) std::cout<<"The node is NOT observable, constructing horrendous node controller \n"<<std::endl;
        // Compute a high stationary cov at node state
        int stateDim = si_->getStateDimension();
        arma::mat stationaryCovariance = arma::eye(stateDim,stateDim)*ompl::magic::NON_OBSERVABLE_NODE_COVARIANCE;

        // create a node controller
        std::vector<ompl::control::Control*> dummyControl;
        std::vector<ompl::base::State*> dummyStates;
        NodeControllerType ctrlr(node, dummyStates, dummyControl, siF_);

        // assign the node controller
        nodeController = ctrlr;
    }
}

template<typename Map>
arma::colvec MapToColvec(const Map& _m) {
  arma::colvec mapData(_m.size());
  int ix = 0;
  for(typename Map::const_iterator i= _m.begin(), e= _m.end(); i!=e; ++i, ++ix)
    mapData[ix] = i->second;
  return mapData;
}

void FIRM::solveDynamicProgram(FIRM::Vertex goalVertex)
{
    using namespace arma;

    if(debug_) std::cout << "Started solving Dynamic Optimization (Programming)" << std::endl;

    float discountFactor = ompl::magic::DYNAMIC_PROGRAMMING_DISCOUNT_FACTOR;

    std::map<Vertex, double> newCostToGo;

    costToGo_ = std::map<Vertex, double>();


    //--NOTES FROM PMPL--
    //For nodes that are not in the goal CC we should assign a high cost to go to all nodes initially,
    //twe assign goalcosttogo and initcosttogo for those nodes that are in the CC of the goal
    //We do this to take care of those nodes that are not in GOAL CC (for replanning purposes)

    foreach (Vertex v, boost::vertices(g_))
    {
        if(v == goalVertex)
        {
            costToGo_[v] = ompl::magic::GOAL_COST_TO_GO;
            newCostToGo[v] = ompl::magic::GOAL_COST_TO_GO;
        }
        else
        {
            costToGo_[v] = ompl::magic::INIT_COST_TO_GO;
            newCostToGo[v] = ompl::magic::INIT_COST_TO_GO;
        }
    }

    feedback_.clear();

    int numIters = 1;
    //double convergenceDiff = norm(newValues - m_values,"inf");
    bool convergenceCondition = false;

    //typedef typename map<VID, double>::iterator CostToGoIT;
    int nIter=0;
    while(!convergenceCondition)
    {
        nIter++;

        if(debug_) cout<<" Iteration in DP :"<<nIter<<endl;

        foreach(Vertex v, boost::vertices(g_))
        {
            //value for goal node stays the same
            if( v == goalVertex)
            {
                continue;
            }

            // Update the costToGo of vertex
            std::pair<Edge,double> candidate = getUpdatedNodeCostToGo(v);

            feedback_[v] = candidate.first;
            newCostToGo[v] = candidate.second * discountFactor;
        }

        convergenceCondition = (norm(MapToColvec(costToGo_)-MapToColvec(newCostToGo), "inf") <= ompl::magic::DP_CONVERGENCE_THRESHOLD);

        if(debug_)
        {
            cout<<" The new computed costToGo  :"<<endl<<MapToColvec(newCostToGo)<<endl;
            //cout<<"Press Enter"<<endl;
            //std::cin.get();
        }

        costToGo_.swap(newCostToGo);   // Equivalent to "m_costToGo = newCostToGo"

    }

    if(debug_) cout<<"DP Solved "<<endl;
}


// We need the following comparison function defined over "map keys" to construct map.
// Note this is NOT used in finding the best edge.
struct EdgeComp
{
  template<typename EID1, typename EID2>
  bool operator()(const EID1& e1, const EID2& e2) const
  {
    if(e1.target() < e2.target())
      return true;
    if(e1.target() > e2.target())
      return false;
    return e1.source() < e2.source();
  }
};


struct DoubleValueComp {
  template<typename KVP1, typename KVP2>
  bool operator()(const KVP1& kvp1, const KVP2& kvp2) const {
    return kvp1.second < kvp2.second;
  }
};


std::pair<typename FIRM::Edge,double> FIRM::getUpdatedNodeCostToGo(FIRM::Vertex node)
{

    std::map<Edge,double /*, EdgeComp*/> candidateCostToGo;

    foreach(Edge e, boost::out_edges(node, g_))
    {
        // the target of given edge "e"
        Vertex targetNode = boost::target(e, g_);

        double nextNodeCostToGo = costToGo_[targetNode];

        FIRMWeight edgeWeight =  boost::get(boost::edge_weight, g_, e);

        double transitionProbability  = edgeWeight.getSuccessProbability();

        double singleCostToGo = ( transitionProbability*nextNodeCostToGo + (1-transitionProbability)*ompl::magic::OBSTACLE_COST_TO_GO) + edgeWeight.getCost();

        candidateCostToGo[e] =  singleCostToGo ;
    }

    DoubleValueComp dvc;
    std::pair<Edge,double> bestCandidate =   *min_element(candidateCostToGo.begin(), candidateCostToGo.end(), dvc );

    if(debug_) std::cout<<" The best candidate cost is: "<<bestCandidate.second<<std::endl;
    //cin.get();
    return bestCandidate;

}

void FIRM::executeFeedback(void)
{
    Vertex start = startM_[0];
    Vertex goal  = goalM_[0] ;

    Vertex currentVertex =  start;

    EdgeControllerType controller;

    ompl::base::State *endState = si_->allocState();

    if(debug_) OMPL_INFORM("Running policy execution");

    while(currentVertex != goal)
    {
        Edge e = feedback_[currentVertex];
        controller = edgeControllers_[e];
        ompl::base::Cost cost;

        if(controller.Execute(stateProperty_[currentVertex], endState, cost, false))
        {
            currentVertex = boost::target(e, g_);
            sleep(1);

        }

    }

    if(debug_)
    {
        using namespace std;

        cout<<"The final filtered State from controller is :"<<endState->as<SE2BeliefSpace::StateType>()->getArmaData()<<endl;
        //cout<<"the final commanded state was :"<<stateProperty_[goal]->as<SE2BeliefSpace::StateType>()->getArmaData()<<endl;

    }
}