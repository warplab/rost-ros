#include <array>
#include <vector>
#include <functional>
#include <algorithm>
#include <unordered_map>
#include <random>
#include <iostream>
#include <fstream>
#include <sstream>
#include <cassert>
#include <mutex>
#include <memory>
#include <thread>
#include <iterator>
#include <atomic>
using namespace std;


typedef array<int,6> Pose6i;
typedef array<int,3> Pose3i;
typedef array<int,1> Posei;


ostream& operator<<(ostream& out, const array<int,3>& v){
  for(auto a : v){
    out<<a<<" ";
  }  
  return out;
}
ostream& operator<<(ostream& out, const array<int,6>& v){
  for(auto a : v){
    out<<a<<" ";
  }  
  return out;
}


/*ostream& operator<<(ostream& out, const Pose6i& v){
  for(auto a : v){
    out<<a<<" ";
  }  
  return out;
  }*/


//maps the v to [-N,N)
template<typename T>
T standardize_angle( T v, T N=180){  
  T r = v%(2*N);
  r = r >= N ? r-2*N: r; 
  r = r < -N ? r+2*N: r;
  return r;
}


//neighbors functor returns the neighbors of the given pose 
//template<typename T>
//struct neighbors{
//  vector<T> operator()(const T &o) const;
//};

//neighbors specialization for a pose of array type
template<typename Container>
struct neighbors {
  const int depth;
  neighbors( int depth_):depth(depth_){}
  vector<Container> operator()(const Container& o) const{

    vector<Container> neighbor_list(o.size()*2*depth, o);
    auto i = neighbor_list.begin();
    
    auto outit = neighbor_list.begin();
    for(size_t i=0; i<o.size(); ++i){
      for(int d = 0; d<depth; ++d){
	(*outit++)[i] += d+1;
	(*outit++)[i] -= d+1;
      }
    }
    return neighbor_list;
  }
};
template<>
struct neighbors<int> {
  int depth;
  neighbors(int depth_):depth(depth_){}
  vector<int> operator()(int o) const{
    vector<int> neighbor_list(2*depth,o);
    auto i = neighbor_list.begin();
    for(int d=1;d<=depth; ++d){
      (*i++)+=d;
      (*i++)-=d;
    }
    return neighbor_list;
  }
};


template<typename T>
struct hash_container{
  hash<typename T::value_type> hash1;
  size_t operator()(const T& pose) const {
    size_t h(0);
    for(size_t i=0;i<pose.size(); ++i){
      if(sizeof(size_t)==4){
      	h = (h<<7) | (h >> 25);
      }
      else{
	h = (h<<11) | (h >> 53);
      }
      h = hash1(pose[i]) ^ h;
    }
    return h;
  }
};


/*template<>
struct hash_container<int>{
  hash<int> hash1;
  size_t operator()(int pose) const {
  return hash1(pose);
  }
};
*/

struct Cell{
  size_t id;
  vector<size_t> neighbors;
  vector<int> W; //word labels
  vector<int> Z; //topic labels
  vector<int> nW; //distribution/count of W
  vector<int> nZ; //distribution/count of Z
  mutex cell_mutex;
  vector<mutex> Z_mutex;
  Cell(size_t id_, size_t vocabulary_size, size_t topics_size):
    id(id_),
    nW(vocabulary_size, 0),
    nZ(topics_size, 0),
    Z_mutex(topics_size)
  {
  }
  vector<int> get_topics(){
    lock_guard<mutex> lock(cell_mutex);
    return Z;
  }
  pair<int, int> get_wz(int i){
    cell_mutex.lock();
    auto r=make_pair(W[i],Z[i]);
    cell_mutex.unlock();
    return r;
  }
  void relabel(size_t i, int z_old, int z_new){
    if(z_old == z_new)
      return;
    lock(Z_mutex[z_old], Z_mutex[z_new]);
    Z[i]=z_new;
    nZ[z_old]--;
    nZ[z_new]++;    
    Z_mutex[z_old].unlock();  Z_mutex[z_new].unlock();
  }
};



template<typename PoseT, 
	 typename PoseNeighborsT=neighbors<PoseT>, 
	 typename PoseHashT=hash_container<PoseT> >
struct ROST{
  PoseNeighborsT neighbors; //function to compute neighbors
  PoseHashT pose_hash;
  unordered_map<PoseT, size_t , PoseHashT> cell_lookup;
  vector<shared_ptr<Cell>> cells;
  mutex cells_mutex;     //lock for cells, since cells can grow in size
  size_t V, K, C;        //vocab size, topic size, #cells
  double alpha, beta;
  mt19937 engine;
  uniform_int_distribution<int> uniform_K_distr;
  vector<vector<int>> nZW; //nZW[z][w] = freq of words of type w in topic z
  vector<int> weight_Z;
  vector<mutex> global_Z_mutex;
  atomic<size_t> refine_count; //number of cells refined till now;

  ROST(size_t V_, size_t K_, double alpha_, double beta_, const PoseNeighborsT& neighbors_ = PoseNeighborsT(), const PoseHashT& pose_hash_ = PoseHashT()):
    neighbors(neighbors_),
    pose_hash(pose_hash_),
    cell_lookup(1000000, pose_hash),
    V(V_), K(K_), C(0),
    alpha(alpha_), beta(beta_),
    uniform_K_distr(0,K-1),
    nZW(K,vector<int>(V,0)),
    weight_Z(K,0),
    global_Z_mutex(K),
    refine_count(0)
  {
  }

  //compute maximum likelihood estimate for topics in the cell for the given pose
  vector<int> get_topics_for_pose(const PoseT& pose){
    //lock_guard<mutex> lock(cells_mutex);
    auto cell_it = cell_lookup.find(pose);
    if(cell_it != cell_lookup.end()){ 
      auto c = get_cell(cell_it->second);
      lock_guard<mutex> lock(c->cell_mutex);
      return estimate(*c);
    }
    else
      return vector<int>();
  }

  shared_ptr<Cell> get_cell(size_t cid){
    lock_guard<mutex> lock(cells_mutex);
    return cells[cid];
  }

  size_t get_refine_count(){
    return refine_count.load();
  }
  size_t num_cells(){
    return C;
  }

  void add_count(int w, int z){
    lock_guard<mutex> lock(global_Z_mutex[z]);
    nZW[z][w]++;
    weight_Z[z]++;
  }

  void relabel(int w, int z_old, int z_new){
    //    cerr<<"lock: "<<z_old<<"  "<<z_new<<endl;
    if(z_old == z_new) return;
    //    lock(global_Z_mutex[z_old<z_new?z_old:z_new ], global_Z_mutex[z_old<z_new?z_new:z_old]);
    if(z_old >= static_cast<int>(global_Z_mutex.size())){
      cerr<<"z_old="<<z_old<<endl;
    }
    if(z_new >= static_cast<int>(global_Z_mutex.size())){
      cerr<<"z_new="<<z_new<<endl;
    }
    assert(z_old < static_cast<int>(global_Z_mutex.size()));
    assert(z_new < static_cast<int>(global_Z_mutex.size()));
    if(z_old<z_new){      
      global_Z_mutex[z_old].lock();
      global_Z_mutex[z_new].lock();
    }
    else{
      global_Z_mutex[z_new].lock();
      global_Z_mutex[z_old].lock();
    }
    //    cerr<<"L:"<<z_old<<","<<z_new<<endl;
    nZW[z_old][w]--;
    weight_Z[z_old]--;
    nZW[z_new][w]++;
    weight_Z[z_new]++;
    global_Z_mutex[z_old].unlock();
    global_Z_mutex[z_new].unlock();
    //cerr<<"U:"<<z_old<<","<<z_new<<endl;
  }

  template<typename WordContainer>
  void add_observation(const PoseT& pose, const WordContainer& words){
    auto cell_it = cell_lookup.find(pose);
    bool newcell = false;
    shared_ptr<Cell> c;
    if(cell_it == cell_lookup.end()){
      c = make_shared<Cell>(C,V,K);
      cells_mutex.lock();
      cells.push_back(c);
      cells_mutex.unlock();

      c->cell_mutex.lock();
      //add neighbors to the cell
      for(auto& g : neighbors(pose)){
	auto g_it = cell_lookup.find(g);
	if(g_it != cell_lookup.end()){
	  auto gc = get_cell(g_it->second);
	  //  cerr<<gc->id<<" ";
	  gc->neighbors.push_back(c->id);
	  c->neighbors.push_back(gc->id);
	}
      }
      //      cerr<<endl;
      cell_lookup[pose]=c->id;
      newcell=true;
    }
    else{
      c = get_cell(cell_it -> second);
      c->cell_mutex.lock();
    }


    //do the insertion
    for(auto w : words){
      c->W.push_back(w);
      //generate random topic label
      int z = uniform_K_distr(engine);
      c->Z.push_back(z);
      //update the histograms
      c->nW[w]++; 
      c->nZ[z]++; 
      add_count(w,z);
    }

    if(newcell){
      C++;
    }

    c->cell_mutex.unlock();
  }


  void refine(Cell& c){
    if(c.id >=C)
      return;
    refine_count++;
    vector<int> nZg(K); //topic distribution over the neighborhood (initialize with the cell)

    //accumulate topic histogram from the neighbors
    for(auto gid: c.neighbors){
      if(gid <C){
	auto g = get_cell(gid); 
	transform(g->nZ.begin(), g->nZ.end(), nZg.begin(), nZg.begin(), plus<int>());
      }
    }

    transform(c.nZ.begin(), c.nZ.end(), nZg.begin(), nZg.begin(), plus<int>());

    vector<double> pz(K,0);

    for(size_t i=0;i<c.W.size(); ++i){
      int w = c.W[i];
      int z = c.Z[i];
      nZg[z]--;

      for(size_t k=0;k<K; ++k){
	int nkw = nZW[k][w]-1;        nkw      = nkw       < 0 ? 0: nkw;
	int weight_k = weight_Z[k]-1; weight_k = weight_k  < 0 ? 0: weight_k;
	pz[k] = (nkw+beta)/(weight_k + beta*V) * (nZg[k]+alpha);
      } 
      discrete_distribution<> dZ(pz.begin(), pz.end());
      int z_new = min<int>(dZ(engine),K-1);

      nZg[z_new]++;
      relabel(w,z,z_new);
      c.relabel(i,z,z_new);
    } 
  }

  //estimate maximum likelihood topics for the cell
  vector<int> estimate(Cell& c){
    if(c.id >=C)
      return vector<int>();

    vector<int> nZg(K); //topic distribution over the neighborhood (initialize with the cell)

    //accumulate topic histogram from the neighbors
    for(auto gid: c.neighbors){
      if(gid <C){
	auto g = get_cell(gid); 
	transform(g->nZ.begin(), g->nZ.end(), nZg.begin(), nZg.begin(), plus<int>());
      }
    }
    transform(c.nZ.begin(), c.nZ.end(), nZg.begin(), nZg.begin(), plus<int>());

    vector<double> pz(K,0);
    vector<int> Zc(c.W.size());

    for(size_t i=0;i<c.W.size(); ++i){
      int w = c.W[i];
      int z = c.Z[i];
      nZg[z]--;

      for(size_t k=0;k<K; ++k){
	int nkw = nZW[k][w];      
	int weight_k = weight_Z[k];
	pz[k] = (nkw+beta)/(weight_k + beta*V) * (nZg[k]+alpha);
      } 
      Zc[i]= max_element(pz.begin(), pz.end()) - pz.begin();
    } 
    return Zc;
  }

};


/*
  Following functions are used to coordinate parallel topic refinement
 */


template<typename R>
void dowork_parallel_refine(R* rost,shared_ptr<vector<size_t>> todo, shared_ptr<mutex> m, int thread_id){
  while(true){
    m->lock();
    if(todo->empty()){m->unlock(); break;}
    size_t cid = todo->back(); todo->pop_back();
    //    cerr<<"T:"<<thread_id<<" >"<<cid<<endl;
    m->unlock();
    rost->refine(*(rost->get_cell(cid)));
  }  
}

template<typename R>
void parallel_refine(R* rost, int nt){
  auto todo = make_shared<vector<size_t>>(rost->C);
  for(size_t i=0;i<todo->size(); ++i){
    (*todo)[i]=i;
  }
  random_shuffle(todo->begin(), todo->end());
  auto m = make_shared<mutex>();
  
  vector<shared_ptr<thread>> threads;
  for(int i=0;i<nt;++i){
    //threads.emplace_back(dowork_parallel_refine,rost,todo ,m);
    threads.push_back(make_shared<thread>(dowork_parallel_refine<R>,rost,todo ,m, i));
  }
  
  for(auto&t: threads){
    t->join();
  }
}


template<typename R, typename Stop>
void dowork_parallel_refine_online(R* rost, double tau, int thread_id, Stop stop){
  gamma_distribution<double> gamma1(tau,1.0);
  gamma_distribution<double> gamma2(1.0,1.0);
  
  while(! stop->load() ){
    double r_gamma1 = gamma1(rost->engine), r_gamma2 = gamma2(rost->engine);
    double r_beta = r_gamma1/(r_gamma1+r_gamma2);
    double p_refine_current = generate_canonical<double, 10>(rost->engine);
    if(rost->C > 0){
      size_t cid;
      if(p_refine_current < 0.9 || rost->C < 10){
	cid = floor(r_beta * static_cast<double>(rost->C));
	//cerr<<"global: "<<cid<<endl;
      }
      else{
	cid = rost->C -10 + floor(r_beta * 10);
	//cerr<<"local: "<<cid<<"/"<<rost->C<<endl;
      }
      if(cid >= rost->C) 
	cid = rost->C-1;

      assert(cid < rost->C);
      if(rost->get_cell(cid)->cell_mutex.try_lock()){
	//	cerr<<"T:"<<thread_id<<" >"<<cid<<endl;
	rost->refine(*rost->get_cell(cid));
	rost->get_cell(cid)->cell_mutex.unlock();
      }
    }
  }
}

template<typename R, typename Stop>
vector<shared_ptr<thread>> parallel_refine_online(R* rost, double tau, int nt, Stop stop){  
  cerr<<"Spawning "<<nt<<" worker threads for refining topics"<<endl;
  vector<shared_ptr<thread>> threads;
  for(int i=0;i<nt;++i){
    threads.push_back(make_shared<thread>(dowork_parallel_refine_online<R,Stop>,rost,tau, i, stop));
  }
  return threads;
}

/*
  Word I/O
*/
struct word_reader{
  istream* stream;
  string line;  
  size_t doc_size;
  word_reader(string filename, size_t doc_size_=0):
    doc_size(doc_size_)
  {
    if(filename=="-" || filename == "/dev/stdin"){
      stream  = &std::cin;
    }
    else{
      stream = new ifstream(filename.c_str());
    }
  }  
  vector<int> get(){
    vector<int> words;    
    getline(*stream,line);
    if(*stream){
      stringstream ss(line);
      copy(istream_iterator<int>(ss), istream_iterator<int>(), back_inserter(words));
    }
    return words;
  }
  ~word_reader(){
    if(stream != &std::cin && stream !=NULL){
      delete stream;
      stream=0;
    }
  }
};



