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
*   * Neither the name of the Texas A&M University nor the names of its
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
/* Author: Ali-akbar Agha-mohammadi, Saurav Agarwal */

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

#include "../../include/Visualization/Visualizer.h"
#define foreach BOOST_FOREACH
#define foreach_reverse BOOST_REVERSE_FOREACH

namespace ompl
{
    namespace magic
    {

        /** \brief The number of steps to take for a random bounce
            motion generated as part of the expansion step of PRM. */
        static const unsigned int MAX_RANDOM_BOUNCE_STEPS   = 5;

        /** \brief The number of nearest neighbors to consider by
            default in the construction of the PRM roadmap */
        static const unsigned int DEFAULT_NEAREST_NEIGHBORS = 10;

        /** \brief The time in seconds for a single roadmap building operation (dt)*/
        static const double ROADMAP_BUILD_TIME = 200;

        static const double NUM_MONTE_CARLO_PARTICLES = 5;

        static const double EXTREMELY_HIGH_EDGE_COST = 1e6;

        static const double NON_OBSERVABLE_NODE_COVARIANCE = 1e2;

        static const float DYNAMIC_PROGRAMMING_DISCOUNT_FACTOR = 1;

        static const int DP_MAX_ITERATIONS = 10000;

        static const double GOAL_COST_TO_GO = 0.0;

        static const double INIT_COST_TO_GO = 2.0;

        static const double OBSTACLE_COST_TO_GO = 500;

        static const double DP_CONVERGENCE_THRESHOLD = 1e-3;

        static const double DEFAULT_NEAREST_NEIGHBOUR_RADIUS = 4.0;
    }
}

FIRM::FIRM(const firm::SpaceInformation::SpaceInformationPtr &si, bool debugMode) :
    ompl::base::Planner(si, "FIRM"),
    siF_(si),
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
    minFIRMNodes_ = 25;
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
        //connectionStrategy_ = ompl::geometric::KStarStrategy<Vertex>(boost::bind(&FIRM::milestoneCount, this), nn_, si_->getStateDimension());
        connectionStrategy_ = FStrategy<Vertex>(ompl::magic::DEFAULT_NEAREST_NEIGHBOUR_RADIUS, nn_);
        //connectionStrategy_ = KStrategy<Vertex>(magic::DEFAULT_NEAREST_NEIGHBORS, nn_);
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

void FIRM::growRoadmap(double growTime)
{
    growRoadmap(ompl::base::timedPlannerTerminationCondition(growTime));
}

void FIRM::growRoadmap(const ompl::base::PlannerTerminationCondition &ptc)
{
    if (!isSetup())
        setup();
    if (!sampler_)
        sampler_ = siF_->allocValidStateSampler();

    ompl::base::State *workState = siF_->allocState();
    growRoadmap (ptc, workState);
    siF_->freeState(workState);
}

void FIRM::growRoadmap(const ompl::base::PlannerTerminationCondition &ptc,
                                       ompl::base::State *workState)
{
    using namespace arma;

    while (ptc == false)
    {
        // search for a valid state
        bool found = false;
        bool stateStable = false;

        while (!found && ptc == false)
        {
            unsigned int attempts = 0;
            do
            {
                found = sampler_->sample(workState);
                stateStable = false;
                if(found)
                {

                    ompl::base::State *lsState = si_->cloneState(workState);

                    LinearSystem ls(siF_, lsState, siF_->getMotionModel()->getZeroControl(),
                                siF_->getObservationModel()->getObservation(lsState, false), siF_->getMotionModel(), siF_->getObservationModel());

                    arma::mat S;

                    try
                    {
                        stateStable = dare (trans(ls.getA()),trans(ls.getH()),ls.getG() * ls.getQ() * trans(ls.getG()),
                                ls.getM() * ls.getR() * trans(ls.getM()), S );
                    }
                    catch(int e)
                    {
                        stateStable = false;
                    }

                }
                attempts++;
            } while (attempts < ompl::magic::FIND_VALID_STATE_ATTEMPTS_WITHOUT_TERMINATION_CHECK && !found && !stateStable);
        }
        // add it as a milestone
        if (found && stateStable)
            addStateToGraph(si_->cloneState(workState));
    }
}

void FIRM::checkForSolution(const ompl::base::PlannerTerminationCondition &ptc,
                                            ompl::base::PathPtr &solution)
{
    boost::this_thread::sleep(boost::posix_time::seconds(90));
    while (!ptc && !addedSolution_)
    {
        // Check for any new goal states
        /*if (goal->maxSampleCount() > goalM_.size())
        {
            const ompl::base::State *st = pis_.nextGoal();
            if (st)
                goalM_.push_back(addStateToGraph(si_->cloneState(st)));
        }
        */
        addedSolution_ = existsPolicy(startM_, goalM_, solution);

        if (!addedSolution_)
            boost::this_thread::sleep(boost::posix_time::seconds(30));
    }
}

bool FIRM::existsPolicy(const std::vector<Vertex> &starts, const std::vector<Vertex> &goals, ompl::base::PathPtr &solution)
{
    ompl::base::Goal *g = pdef_->getGoal().get();
    ompl::base::Cost sol_cost(0.0);

    if(boost::num_vertices(g_) < minFIRMNodes_) return false;

    foreach (Vertex start, starts)
    {
        foreach (Vertex goal, goals)
        {
            graphMutex_.lock();
            bool same_component = sameComponent(start, goal);
            graphMutex_.unlock();

            if (same_component && g->isStartGoalPairValid(stateProperty_[goal], stateProperty_[start]))
            {
                boost::mutex::scoped_lock _(graphMutex_);
                solveDynamicProgram(goal);
                solution = constructFeedbackPath(start, goal);
                sendFeedbackEdgesToViz();
                return true; // return true if solution is found
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
        startM_.push_back(addStateToGraph(si_->cloneState(st)));

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
            goalM_.push_back(addStateToGraph(si_->cloneState(st)));

        if (goalM_.empty())
        {
            OMPL_ERROR("%s: Unable to find any valid goal states", getName().c_str());
            return ompl::base::PlannerStatus::INVALID_GOAL;
        }
    }

    unsigned int nrStartStates = boost::num_vertices(g_);
    OMPL_INFORM("%s: Starting with %u states", getName().c_str(), nrStartStates);

    addedSolution_ = false;
    ompl::base::PathPtr sol;
    boost::thread slnThread(boost::bind(&FIRM::checkForSolution, this, ptc, boost::ref(sol)));

    ompl::base::PlannerTerminationCondition ptcOrSolutionFound =
        ompl::base::plannerOrTerminationCondition(ptc, ompl::base::PlannerTerminationCondition(boost::bind(&FIRM::addedNewSolution, this)));

    constructRoadmap(ptcOrSolutionFound);

    slnThread.join();

    OMPL_INFORM("%s: Created %u states", getName().c_str(), boost::num_vertices(g_) - nrStartStates);

    if (sol)
    {
        ompl::base::PlannerSolution psol(sol);
        // if the solution was optimized, we mark it as such
        if (addedNewSolution())
            psol.optimized_ = true;
        pdef_->addSolutionPath(psol);
    }

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

    while (ptc() == false)
    {
        growRoadmap(ompl::base::plannerOrTerminationCondition(ptc, ompl::base::timedPlannerTerminationCondition(ompl::magic::ROADMAP_BUILD_TIME)), xstates[0]);
    }

    si_->freeStates(xstates);
}

FIRM::Vertex FIRM::addStateToGraph(ompl::base::State *state, bool addReverseEdge, bool shouldCreateNodeController)
{

    boost::mutex::scoped_lock _(graphMutex_);

    Vertex m = boost::add_vertex(g_);

    addStateToVisualization(state);

    stateProperty_[m] = state;
    NodeControllerType nodeController;
    generateNodeController(state, nodeController); // Generate the node controller
    nodeControllers_[m] = nodeController; // Add it to the list

    totalConnectionAttemptsProperty_[m] = 1;
    successfulConnectionAttemptsProperty_[m] = 0;

    // Initialize to its own (dis)connected component.
    disjointSets_.make_set(m);

    nn_->add(m);

    // Which milestones will we attempt to connect to?
    const std::vector<Vertex>& neighbors = connectionStrategy_(m);

    foreach (Vertex n, neighbors)
    {
        if ( m!=n && stateProperty_[m]!=stateProperty_[n])
        {
            totalConnectionAttemptsProperty_[m]++;
            totalConnectionAttemptsProperty_[n]++;
            if (si_->checkMotion(stateProperty_[m], stateProperty_[n]))
            {
                successfulConnectionAttemptsProperty_[m]++;
                successfulConnectionAttemptsProperty_[n]++;

                addEdgeToGraph(m, n);

                if(addReverseEdge)
                {
                    addEdgeToGraph(n, m);
                }

                uniteComponents(m, n);
            }
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

ompl::base::PathPtr FIRM::constructFeedbackPath(const Vertex &start, const Vertex &goal)
{
    FeedbackPath *p = new FeedbackPath(siF_);

    Vertex currentVertex = start;

    while(currentVertex!=goal)
    {
        Edge edge = feedback_[currentVertex]; // get the edge
        Vertex target = boost::target(edge, g_); // get the target of this edge
        p->append(stateProperty_[currentVertex],edgeControllers_[edge]); // push the state and controller to take
        if(target == goal)
        {

            p->append(stateProperty_[target]); // because from the goal node you don't need to take a controller
        }
        currentVertex =  target;
    }

    return ompl::base::PathPtr(p);
}

void FIRM::addEdgeToGraph(const FIRM::Vertex a, const FIRM::Vertex b)
{

    EdgeControllerType edgeController;

    const FIRMWeight weight = generateEdgeControllerWithCost(stateProperty_[a], stateProperty_[b], edgeController);

    assert(edgeController.getGoal() && "The generated controller has no goal");

    const unsigned int id = maxEdgeID_++;

    const Graph::edge_property_type properties(weight, id);

    // create an edge with the edge weight property
    std::pair<Edge, bool> newEdge = boost::add_edge(a, b, properties, g_);

    edgeControllers_[newEdge.first] = edgeController;

    Visualizer::addGraphEdge(stateProperty_[a], stateProperty_[b]);
}

FIRMWeight FIRM::generateEdgeControllerWithCost(ompl::base::State* startNodeState, ompl::base::State* targetNodeState, EdgeControllerType &edgeController)
{

    double successCount = 0;

    // initialize costs to 0
    ompl::base::Cost edgeCost(0);
    ompl::base::Cost nodeStabilizationCost(0);

    // Generate the edge controller for given start and end state
    generateEdgeController(startNodeState,targetNodeState,edgeController);

    for(unsigned int i=0; i< numParticles_;i++)
    {
        //cout << "MonteCarlo Simulation particle number "<< i<<endl;
        siF_->setTrueState(startNodeState);
        siF_->setBelief(startNodeState);

        ompl::base::State* endBelief = siF_->allocState(); // allocate the end state of the controller
        ompl::base::Cost pcost(0);

        if(edgeController.Execute(startNodeState, endBelief, pcost))
        {
           successCount++;

           edgeCost.v = edgeCost.v + pcost.v ;

        }

    }

    if (successCount > 0)  edgeCost.v = edgeCost.v / successCount ;
    else edgeCost.v = ompl::magic::EXTREMELY_HIGH_EDGE_COST; // extremely high cost if no particle could succeed, we can also simply not add this edge

    double transitionProbability = successCount / numParticles_ ;

    FIRMWeight weight(edgeCost.v, transitionProbability);

    return weight;
}

void FIRM::generateEdgeController(const ompl::base::State *start, const ompl::base::State* target, FIRM::EdgeControllerType &edgeController)
{
    assert(target && "The target state for generating the edge controller is NULL");
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
        // Contruct a linear kalman filter
        LinearizedKF linearizedKF(siF_);

        //Construct a linear system
        LinearSystem linearSystem(siF_, node, siF_->getMotionModel()->getZeroControl(),siF_->getObservationModel()->getObservation(state, false), siF_->getMotionModel(), siF_->getObservationModel());

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

void FIRM::solveDynamicProgram(const FIRM::Vertex goalVertex)
{
    OMPL_INFORM("Solving DP");

    using namespace arma;

    float discountFactor = ompl::magic::DYNAMIC_PROGRAMMING_DISCOUNT_FACTOR;

    std::map<Vertex, double> newCostToGo;

    costToGo_ = std::map<Vertex, double>();

    /**
    --NOTES--
    Assign a high cost to go initially for all nodes that are not in the goal connected component.
    For nodes that are in the goal cc, we assign goal cost to go for the goal and init cost to go
    for all other nodes.
    */
    foreach (Vertex v, boost::vertices(g_))
    {
        if(boost::out_degree(v,g_) > 0 )
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
    }

    feedback_.clear();

    bool convergenceCondition = false;

    int nIter=0;
    while(!convergenceCondition && nIter < ompl::magic::DP_MAX_ITERATIONS)
    {
        nIter++;

        foreach(Vertex v, boost::vertices(g_))
        {
            //value for goal node stays the same or if has no out edges then ignore it
            if( v == goalVertex || boost::out_degree(v,g_) < 1 )
            {
                continue;
            }

            // Update the costToGo of vertex
            std::pair<Edge,double> candidate = getUpdatedNodeCostToGo(v);

            feedback_[v] = candidate.first;

            newCostToGo[v] = candidate.second * discountFactor;
        }

        convergenceCondition = (norm(MapToColvec(costToGo_)-MapToColvec(newCostToGo), "inf") <= ompl::magic::DP_CONVERGENCE_THRESHOLD);

        costToGo_.swap(newCostToGo);   // Equivalent to costToGo_ = newCostToGo

    }

}


struct DoubleValueComp
{
  template<typename KVP1, typename KVP2>
  bool operator()(const KVP1& kvp1, const KVP2& kvp2) const
  {
    return kvp1.second < kvp2.second;
  }
};


std::pair<typename FIRM::Edge,double> FIRM::getUpdatedNodeCostToGo(const FIRM::Vertex node)
{

    std::map<Edge,double > candidateCostToGo;

    foreach(Edge e, boost::out_edges(node, g_))
    {
        // the target of given edge "e"
        const Vertex targetNode = boost::target(e, g_);

        double nextNodeCostToGo = costToGo_[targetNode];

        FIRMWeight edgeWeight =  boost::get(boost::edge_weight, g_, e);

        double transitionProbability  = edgeWeight.getSuccessProbability();

        double singleCostToGo = ( transitionProbability*nextNodeCostToGo + (1-transitionProbability)*ompl::magic::OBSTACLE_COST_TO_GO) + edgeWeight.getCost();

        candidateCostToGo[e] =  singleCostToGo ;

        //candidateCostToGo.insert(std::pair<Edge,double>(e,singleCostToGo));
    }

    DoubleValueComp dvc;
    std::pair<Edge,double> bestCandidate =   *min_element(candidateCostToGo.begin(), candidateCostToGo.end(), dvc );

    return bestCandidate;

}

void FIRM::executeFeedback(void)
{
    sendFeedbackEdgesToViz();

    Vertex start = startM_[0];
    Vertex goal  = goalM_[0] ;

    siF_->setTrueState(stateProperty_[start]);
    siF_->setBelief(stateProperty_[start]);

    Vertex currentVertex =  start;

    EdgeControllerType controller;

    ompl::base::State *cstartState = si_->allocState();
    si_->copyState(cstartState, stateProperty_[start]);

    ompl::base::State *cendState = si_->allocState();

    OMPL_INFORM("Running policy execution");

    while(currentVertex != goal)
    {
        Edge e = feedback_[currentVertex];
        controller = edgeControllers_[e];
        ompl::base::Cost cost;

        if(controller.Execute(cstartState, cendState, cost, false))
        {
            currentVertex = boost::target(e, g_);
        }
        else
        {
            // get a copy of the true state
            ompl::base::State *tempTrueStateCopy = si_->allocState();

            siF_->getTrueState(tempTrueStateCopy);

            currentVertex = addStateToGraph(cendState);

            // Set true state back to its correct value after Monte Carlo (happens during adding state to Graph)
            siF_->setTrueState(tempTrueStateCopy);

            siF_->freeState(tempTrueStateCopy);

            solveDynamicProgram(goal);
        }
        si_->copyState(cstartState, cendState);

        if(si_->distance(cstartState,stateProperty_[goal]) < 5.0)
        {
            std::cout<<"Before Simulated Kidnapping! (Press Enter) \n";
            std::cin.get();
            this->simulateKidnapping();
            std::cout<<"AFter Simulated Kidnapping! (Press Enter) \n";
            std::cin.get();
        }

    }


}

void FIRM::executeFeedbackWithRollout(void)
{
    sendFeedbackEdgesToViz();

    Vertex start = startM_[0];
    Vertex goal  = goalM_[0] ;

    siF_->setTrueState(stateProperty_[start]);
    siF_->setBelief(stateProperty_[start]);

    Vertex currentVertex =  start;

    EdgeControllerType controller;

    ompl::base::State *cstartState = si_->allocState();
    si_->copyState(cstartState, stateProperty_[start]);

    ompl::base::State *cendState = si_->allocState();

    OMPL_INFORM("Running policy execution");

    Edge e = feedback_[currentVertex];

    Vertex tempVertex;

    while(si_->distance(stateProperty_[currentVertex], stateProperty_[goal]) > 0.5)
    {
        //Edge e = feedback_[currentVertex];
        //Vertex targetNode = boost::target(e, g_);

        controller = edgeControllers_[e];

        if(tempVertex)
        {
            boost::remove_vertex(tempVertex, g_);
        }

        ompl::base::Cost cost;

        /**
            Instead of executing the entire controller, we need to
            execute one step, then calculate the cost to go through the neighboring nodes.
            Whichever gives the lowest cost to go, is our new path.
            Do this at each step.
        */
        controller.executeUpto(100,cstartState,cendState,cost,false);

        ompl::base::State *tState = si_->allocState();

        siF_->getTrueState(tState);

        tempVertex = addStateToGraph(cendState, false);

        //solveDynamicProgram(goal);

        siF_->setTrueState(tState);

        si_->freeState(tState);

        e = generateRolloutPolicy(tempVertex);

        sendFeedbackEdgesToViz();

        si_->copyState(cstartState, cendState);

    }


}

void FIRM::addStateToVisualization(ompl::base::State *state)
{
    Visualizer::addState(state);
}

void FIRM::sendFeedbackEdgesToViz()
{
    Visualizer::ClearFeedbackEdges();

    for(typename std::map<Vertex, Edge>::const_iterator i=feedback_.begin(), e=feedback_.end(); i!=e; ++i)
    {
        Vertex sourceVertex, targetVertex;
        Edge edge;
        sourceVertex = i->first;
        edge = i->second;
        targetVertex = boost::target(edge, g_);
        //TODO: some random target vertex which is not in graph is being assigned, why?
        Visualizer::addFeedbackEdge(stateProperty_[sourceVertex], stateProperty_[targetVertex], 0);
  }
}


FIRM::Edge FIRM::generateRolloutPolicy(const FIRM::Vertex currentVertex)
{
    /**
        For the given node, find the out edges and see the total cost of taking that edge

        The cost of taking the edge is cost to go from the target of the edge + the cost of the edge itself
    */
    double minCost = 1e10;
    Edge edgeToTake;
    // Iterate over the out edges
    foreach(Edge e, boost::out_edges(currentVertex, g_))
    {

        // Get the target node of the edge
        Vertex targetNode = boost::target(e, g_);

        // The cost to go from the target node
        double nextNodeCostToGo = costToGo_[targetNode];

        // Find the weight of the edge
        FIRMWeight edgeWeight =  boost::get(boost::edge_weight, g_, e);

        // The transition prob of the edge
        double transitionProbability  = edgeWeight.getSuccessProbability();

        // the cost of taking the edge
        double edgeCostToGo = (transitionProbability*nextNodeCostToGo + (1-transitionProbability)*ompl::magic::OBSTACLE_COST_TO_GO) + edgeWeight.getCost();

        if(edgeCostToGo < minCost)
        {
            minCost  = edgeCostToGo;
            edgeToTake = e;

        }

    }

    return edgeToTake;
}

void FIRM::simulateKidnapping()
{
    //Kidnapped state
    double X = 2.0;
    double Y = 19.5;
    double theta = 1.57;

    ompl::base::State *kidnappedState = si_->allocState();

    //ompl::base::State *currentRobotState = si_->allocState();

    //siF_->getTrueState(currentRobotState);

    //arma::mat currentCovariance = currentRobotState->as<SE2BeliefSpace::StateType>()->getCovariance();

    kidnappedState->as<SE2BeliefSpace::StateType>()->setXYYaw(X,Y,theta);
    //kidnappedState->as<SE2BeliefSpace::StateType>()->setCovariance(currentCovariance);

    siF_->setTrueState(kidnappedState);

}
