#include "sim2d.h"

#include <cmath>
#include <QDebug>
#include <QImage>
#include <math.h>
#include <cmath>
#include <algorithm>
#include <QPainter>
#include <stdlib.h>
#include <QColor>
#include <stdio.h>
#include <time.h>
#include <chrono>
#include <omp.h>
#include <QRgb>
#include <vector>
#include <utility>

// inits mesh with given mass and size,
// is [dSprings] is true, then mesh gets diagonal springs
Sim2D::Sim2D(uint _m, double _meshMass, double _meshSize, bool dSprings) {
  this->h = 0.3;
  this->m = _m;
  this->simStep = 0;
  this->iterationsPerStep = 5;
  this->imgViewSize = 0;
  this->springForceConstant = 2.5;
  this->timeInSim = 0;
  this->gravRefHeight = 0;
  this->minSpringLen = 1e99;
  this->meshMass = _meshMass;

  this->q = VectorXd(2 * m);
  this->v = VectorXd::Zero(2 * m);
  this->F = VectorXd::Zero(2 * m);
  this->F_floor = VectorXd::Zero(2 * m);
  this->F_grav = VectorXd::Zero(2 * m);

  this->M = SparseMatrix <double>(2 * m, 2 * m);
  this->M.setIdentity( );
  this->M = this->M * (meshMass / double(m));
  this->Minv = SparseMatrix <double>(2 * m, 2 * m);

  for (uint i = 0; i < 2 * m; i++) {
    Minv.coeffRef(i, i) = 1.0 / M.coeffRef(i, i);
  } // for

  InitMesh_Square(_meshSize, dSprings); // init edges and stuff
  InitRestLen( ); // compute rest lengths
  InitPs( ); // init pVec
} // Sim2D

// inits mesh with given mass and size from image, each non-black pixel is a vertex
// is [dSprings] is true, then mesh gets diagonal springs
Sim2D::Sim2D(QImage img, double _meshMass, double _meshSize, bool dSprings) {
  qDebug( ) << "Start loading mesh from image...";
  this->m = 0;
  uint imgW = img.width( ), imgH = img.height( );
  uint L = max(imgW, imgH);
  uint vNr;
  double scale = _meshSize / L;

  // mMap contains indices of vertices for each location
  vector < vector <int> > mMap(imgW, vector <int> (imgH, -1));
  QColor col;
  // determine number of vertices [m]:
  for (uint x = 1; x < imgW - 1; x++)
    for (uint y = 1; y < imgH - 1; y++) {
      col = img.pixel(x, y);
      if (col.red( ) > 0) { // non-black pixel
        mMap[x][y] = m;
        m++;
      } // if
    } // for

  this->q = VectorXd(2 * m);
  this->v = VectorXd::Zero(2 * m);
  this->F = VectorXd::Zero(2 * m);
  this->F_floor = VectorXd::Zero(2 * m);
  this->F_grav = VectorXd::Zero(2 * m);

  // add vertices:
  for (uint x = 1; x < imgW - 1; x++)
    for (uint y = 1; y < imgH - 1; y++) {
      if (mMap[x][y] != -1) {
        vNr = mMap[x][y];
        this->q(2 * vNr)     = double(x) * scale;
        this->q(2 * vNr + 1) = double(y) * scale;
      } // if
    } // for

  qDebug( ) << "Added" << m << "vertices";

  // add edges:
  vector < pair <uint, uint> > eVec;
  for (uint x = 1; x < imgW - 1; x++) {
    for (uint y = 1; y < imgH - 1; y++) {
      if (mMap[x][y] != -1) {
        vNr = mMap[x][y];
        if (mMap[x+1][y] != -1) // vertex on right
          eVec.push_back(make_pair(vNr, mMap[x+1][y]));
        if (mMap[x][y+1] != -1) // vertex below
          eVec.push_back(make_pair(vNr, mMap[x][y+1]));

        if (dSprings) { // add diagonal springs
          if (mMap[x+1][y+1] != -1) // vertex below right
            eVec.push_back(make_pair(vNr, mMap[x+1][y+1]));
          if (mMap[x+1][y-1] != -1) // vertex above right
            eVec.push_back(make_pair(vNr, mMap[x+1][y-1]));
        } // if
      } // if
    } // for
  } // for
  this->numEdges = eVec.size( );
  this->E = MatrixXi(numEdges, 2);
  for (uint i = 0; i < numEdges; i++) {
    E(i, 0) = eVec[i].first;
    E(i, 1) = eVec[i].second;
  } // for
  qDebug( ) << "Added" << numEdges << "edges";

  this->h = 0.3;
  this->simStep = 0;
  this->iterationsPerStep = 5;
  this->imgViewSize = 0;
  this->springForceConstant = 2.5;
  this->timeInSim = 0;
  this->gravRefHeight = 0;
  this->minSpringLen = 1e99;
  this->meshMass = _meshMass;

  this->M = SparseMatrix <double>(2 * m, 2 * m);
  this->M.setIdentity( );
  this->M = this->M * (meshMass / double(m));

  this->Minv = SparseMatrix <double>(2 * m, 2 * m);
  for (uint i = 0; i < 2 * m; i++)
    Minv.coeffRef(i, i) = 1.0 / M.coeffRef(i, i);

  InitRestLen( ); // compute rest lengths
  InitPs( ); // init pVec

  qDebug( ) << "Done loading mesh from image...";
} // Sim2D

Sim2D::~Sim2D( ) {

} // ~Sim2D


// sets value of F_floor
void Sim2D::ComputeFloorForce( ) {
  this->F_floor = VectorXd::Zero(2 * m);
  if (floorEnabled == false) // no floor
    return;

  double f, y, dy;
  for (uint i = 0; i < m; i++) {
    y = q(2 * i + 1);
    dy = floorHeight - floorDist - y; // positive if above floorDist
    if (dy >= 0) // vertex too far away
      continue;

    f = dy * floorC;
    F_floor(2 * i + 1) += f;
  } // for
} // ComputeFloorForce

// returns min x pos in q
double Sim2D::MinX( ) {
  double ans = 1e99;
  for (uint i = 0; i < m; i++)
    if (q(2 * i) < ans)
      ans = q(2 * i);
  return ans;
} // MinX

// returns min y pos in q
double Sim2D::MinY( ) {
  double ans = 2e99;
  for (uint i = 0; i < m; i++)
    if (q(2 * i + 1) < ans)
      ans = q(2 * i + 1);
  return ans;
} // MinY

// returns max x pos in q
double Sim2D::MaxX( ) {
  double ans = -1e99;
  for (uint i = 0; i < m; i++)
    if (q(2 * i) > ans)
      ans = q(2 * i);
  return ans;
} // MaxX

// returns max y pos in q
double Sim2D::MaxY( ) {
  double ans = -1e99;
  for (uint i = 0; i < m; i++)
    if (q(2 * i + 1) > ans)
      ans = q(2 * i + 1);
  return ans;
} // MaxY

// returns y-pos of COM; sum of y_i * M_i,
// y-pos of node times mass of node
double Sim2D::GetCenterOfMassY( ) {
  double ans = 0;
  double totMass = 0, mass;
  for (uint i = 0; i < m; i++) {
    mass = this->M.coeff(2 * i, 2 * i);
    totMass += mass;
    ans += q(2 * i + 1) * mass;
  } // for

  return ans / totMass;
} // GetCenterOfMassY

// returns x-pos of COM; sum of x_i * M_i,
// x-pos of node times mass of node
double Sim2D::GetCenterOfMassX( ) {
  double ans = 0;
  double totMass = 0, mass;
  for (uint i = 0; i < m; i++) {
    mass = this->M.coeff(2 * i, 2 * i);
    totMass += mass;
    ans += q(2 * i) * mass;
  } // for

  return ans / totMass;
} // GetCenterOfMassX

// returns velocity of vertex k
double Sim2D::GetV(int k) {
  int size = this->v.rows( );
  if (k < 0 || k >= size) {
    qDebug( ) << "GetV err, n =" << k << ", size =" << size;
    return 0;
  } // if
  double vx = v(2 * k);
  double vy = v(2 * k + 1);
  return sqrt(vx*vx + vy*vy);
} // GetV

// returns max of vertex velocities
double Sim2D::MaxV( ) {
  double ans = -1e99;
  for (uint i = 0; i < m; i++)
    if (GetV(i) > ans)
      ans = GetV(i);
  return ans;
} // MaxV

// returns min of vertex velocities
double Sim2D::MinV( ) {
  double ans = 1e99;
  for (uint i = 0; i < m; i++)
    if (GetV(i) < ans)
      ans = GetV(i);
  return ans;
} // MinV

// returns absolute uitwijking of spring k
double Sim2D::GetU(int k) {
  uint v1 = pVec[k].v1;
  uint v2 = pVec[k].v2;
  double dx = q(2 * v1) - q(2 * v2);
  double dy = q(2 * v1 + 1) - q(2 * v2 + 1);
  double l = sqrt(dx*dx + dy*dy);
  return fabs(l - pVec[k].r0);
} // GetU

// returns max uitwijking
double Sim2D::MaxU( ) {
  double ans = -1e99;
  for (uint i = 0; i < m; i++)
    if (GetU(i) > ans)
      ans = GetU(i);
  return ans;
} // MaxU

// returns min uitwijking
double Sim2D::MinU( ) {
  double ans = 1e99;
  for (uint i = 0; i < m; i++)
    if (GetU(i) < ans)
      ans = GetU(i);
  return ans;
} // MinU

// returns total kinetic energy of all vertices
double Sim2D::GetKineticEnergy( ) {
  double vx, vy;
  double mass;
  double Ekin = 0;
  for (uint i = 0; i < m; i++) {
    vx = this->v(2 * i);
    vy = this->v(2 * i + 1);
    mass = this->M.coeff(2 * i, 2 * i);
    Ekin += 0.5 * mass * (vx*vx + vy*vy);
  } // for
  return Ekin;
} // GetKineticEnergy

// returns total potential spring energy of all potentials
double Sim2D::GetSpringPotentialEnergy( ) {
  uint v1, v2; // indices of vertices
  double x1, x2, y1, y2;
  double Epot = 0;

  for (auto & pot : pVec) {
    v1 = pot.v1;
    v2 = pot.v2;
    x1 = q(v1 * 2);
    y1 = q(v1 * 2 + 1);
    x2 = q(v2 * 2);
    y2 = q(v2 * 2 + 1);
    Epot += pot.GetPotE(x1, y1, x2, y2);
  } // for
  return Epot;
} // GetSpringPotentialEnergy

// returns total gravitational potential energy of all vertices
double Sim2D::GetGravPotEnergy() {
  double height;
  double mass;
  double Epot = 0;
  for (uint i = 0; i < m; i++) {
    height = this->gravRefHeight - this->q(2 * i + 1);
    mass = this->M.coeff(2 * i, 2 * i);
    Epot += mass * height * this->gravAcc;
  } // for
  return Epot;
} // GetGravPotEnergy

// returns total energy of current state
double Sim2D::GetTotEnergy( ) {
  return GetGravPotEnergy( ) + GetSpringPotentialEnergy( ) + GetKineticEnergy( );
} // GetTotEnergy

// returns string with stats about sim;
// various computation times
QString Sim2D::GetInfoString( ) {
  QString str;
  str += "T to solve q         : " + QString::number(qSolveTime, 'f', 3) + "ms\n";
  str += "T to compute rMatrix : " + QString::number(rMatrixMakeTime, 'f', 3) + "ms\n";
  str += "T to compute p_i's   : " + QString::number(pSolveTime, 'f', 3) + "ms\n\n";

  double Ekin = GetKineticEnergy( );
  double Epot = GetSpringPotentialEnergy( );
  double Egrav = GetGravPotEnergy( );

  str += "Kinetic energy       : " + QString::number(Ekin, 'f', 3) + "\n";
  str += "Potential energy     : " + QString::number(Epot, 'f', 3) + "\n";
  str += "Gravitational energy : " + QString::number(Egrav, 'f', 3) + "\n";
  str += "Total energy         : " + QString::number(Epot + Ekin + Egrav, 'f', 3) + "\n\n";

  str += "Closest spring dist  : " + QString::number(minSpringLen, 'f', 3) + "\n";

  return str;
} // GetInfoString

// arranges vertices in square grid, of which lengths of sides are p_meshSize
// if [dSprings] is true, then mesh gets diagonal springs
void Sim2D::InitMesh_Square(double _meshSize, bool dSprings) {
  uint N = sqrt(m);
  if (N * N != m) { // m not a square
    qDebug( ) << "Error in InitMesh! m not a square";
    return;
  } // if

  double x, y;
  double size = _meshSize; // length of sides

  for (uint i = 0; i < m; i++) {
    x = size * (double(i % N) / double(N-1)) - 0.5 * size;
    y = -size * (double(int(i / (N))) / double(N-1)) + 0.5 * size;

    this->q(2 * i)     = x;
    this->q(2 * i + 1) = y;
  } // for

  // now init edges
  uint nE = 2 * N * (N - 1); // number of edges
  if (dSprings)
    nE += 2 * (N-1) * (N-1); // add diagonals

  this->E = MatrixXi(nE, 2);
  this->numEdges = 0;

  for (uint i = 0; i < m; i++) {
    if ((i + 1) % N != 0) { // connect to vertex on right
      E(numEdges, 0) = i;
      E(numEdges, 1) = i + 1;
      numEdges++;
    } // if
    if (i + N < m) { // connect to vertex below
      E(numEdges, 0) = i;
      E(numEdges, 1) = i + N;
      numEdges++;
    } // if
    if (dSprings) {
      if (i + N + 1 < m && (i + N + 1) % N != 0) { // connect to vertex right below
        E(numEdges, 0) = i;
        E(numEdges, 1) = i + N + 1;
        numEdges++;
      } // if
      if (i + N - 1 < m && i % N != 0) { // connect to vertex left below
        E(numEdges, 0) = i;
        E(numEdges, 1) = i + N - 1;
        numEdges++;
      } // if
    } // if
  } // for

  if (numEdges != nE) { // m not a square
    qDebug( ) << "Error in InitMesh! numEdges =" << numEdges << "!=" << nE;
    return;
  } // if

  double yCOM = this->GetCenterOfMassY( );
  this->gravRefHeight = yCOM;
  //qDebug( ) << "Done with InitMesh_Square. size of q:" << q.rows( ) << ", yCOM:" << yCOM;
} // InitMesh_Square

// fills vector restLen with current length of edges
void Sim2D::InitRestLen( ) {
  //qDebug( ) << "Starting InitRestLen... numEdges =" << numEdges;
  uint v1, v2; // indices of vertices
  double x1, x2, y1, y2;
  double dx, dy;

  this->restLen.clear( );

  for (uint e = 0; e < numEdges; e++) {
    v1 = E(e, 0);
    v2 = E(e, 1);
    x1 = q(v1 * 2);
    y1 = q(v1 * 2 + 1);
    x2 = q(v2 * 2);
    y2 = q(v2 * 2 + 1);
    dx = x2 - x1;
    dy = y2 - y1;
    restLen.push_back(sqrt(dx*dx + dy*dy));
  } // for
  //qDebug( ) << "Finished InitRestLen... size =" << restLen.size( );
} // InitRestLen


void Sim2D::InitPs( ) {
  //qDebug( ) << "InitPs start...";
  pVec.clear( );
  uint v1, v2; // indices of vertices
  for (uint e = 0; e < numEdges; e++) {
    v1 = E(e, 0);
    v2 = E(e, 1);
    SpringPotential2D pot(v1, v2, restLen[e], springForceConstant);
    pVec.push_back(pot);
  } // for
  //qDebug( ) << "InitPs done... pVec.size =" << pVec.size( );
} // InitPs

// compute auxilliary variables and puts them in vector p
void Sim2D::ComputePs( ) {
  auto start = std::chrono::high_resolution_clock::now( );
  auto elapsed = std::chrono::high_resolution_clock::now( ) - start;
  long long microseconds;

  for (auto & pot : pVec) {
    uint v1 = pot.v1;
    uint v2 = pot.v2;
    double x1 = q(v1 * 2);
    double y1 = q(v1 * 2 + 1);
    double x2 = q(v2 * 2);
    double y2 = q(v2 * 2 + 1);
    double dx = x2 - x1;
    double dy = y2 - y1;
    double l = sqrt(dx*dx + dy*dy);

    if (l < minSpringLen)
      minSpringLen = l;

    double fac;
    if (l > 0) {
      fac = pot.r0 / l;
      pot.dx = fac * dx;
      pot.dy = fac * dy;
    } // if
    else {
      qDebug( ) << "l = 0 in ComputePs!";
      pot.dx = pot.r0 / sqrt(2);
      pot.dy = pot.r0 / sqrt(2);
    } // else
  } // for


  elapsed = std::chrono::high_resolution_clock::now( ) - start;
  microseconds = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count( );

  if (simStep > 0)
    this->pSolveTime = 0.999 * this->pSolveTime + 0.001 * (double(microseconds) / 1000);
  else
    this->pSolveTime = double(microseconds) / 1000;
} // ComputePs


// displaces selected vertices randomly
void Sim2D::AddNoise(double strength) {
  srand (time(NULL));
  double noise;
  uint index;

  for (uint i = 0; i < selectedVertices.size( ); i++) {
    index = selectedVertices[i];

    // noise in x-pos
    noise = double(rand( )) / RAND_MAX;
    noise -= 0.5; // in [-0.5 0.5]
    q(2 * index) += noise * strength;
    // noise in y-pos
    noise = double(rand( )) / RAND_MAX;
    noise -= 0.5; // in [-0.5 0.5]
    q(2 * index + 1) += noise * strength;
  } // for
} // AddNoise


// adds random velocity to selected vertices
void Sim2D::AddVNoise(double strength) {
  qDebug( ) << "AddVNoise";
  srand (time(NULL));
  double noise;
  uint index;

  for (uint i = 0; i < selectedVertices.size( ); i++) {
    index = selectedVertices[i];

    if (index >= m-1)
      qDebug( ) << "Err in vNoise, m =" << m << "and index =" << index;

    // noise in x-pos
    noise = double(rand( )) / RAND_MAX;
    noise -= 0.5; // in [-0.5 0.5]
    v(2 * index) += noise * strength;
    // noise in y-pos
    noise = double(rand( )) / RAND_MAX;
    noise -= 0.5; // in [-0.5 0.5]
    v(2 * index + 1) += noise * strength;
  } // for
  qDebug( ) << "Done adding vNoise";
} // AddVNoise


// adds given velocity to selected vertices
void Sim2D::AddVelocity(double vx, double vy) {
  uint index;
  for (uint i = 0; i < selectedVertices.size( ); i++) {
    index = selectedVertices[i];
    v(2 * index) += vx;
    v(2 * index + 1) += vy;
  } // for

  // just in case set locked vertex velocity to zero
  for (uint i = 0; i < lockedVertices.size( ); i++) {
    index = lockedVertices[i];
    v(2 * index) = 0;
    v(2 * index + 1) = 0;
  } // for
} // AddVelocity

// squeezes mesh in x-direction by factor
void Sim2D::SqueezeX(double factor) {
  double xCOM = GetCenterOfMassX( );
  double x, dx;
  for (uint i = 0; i < m; i++) {
    x = this->q(2 * i);
    dx = x - xCOM;
    this->q(2 * i) = xCOM + dx * factor;
  } // for
} // SqueezeX

// squeezes mesh in y-direction by factor
void Sim2D::SqueezeY(double factor) {
  double yCOM = GetCenterOfMassY( );
  double y, dy;
  for (uint i = 0; i < m; i++) {
    y = this->q(2 * i + 1);
    dy = y - yCOM;
    this->q(2 * i + 1) = yCOM + dy * factor;
  } // for
} // SqueezeY

// sets all y-components of forces in F to m*g
void Sim2D::SetGravity(double g) {
  this->gravAcc = g;
  this->F_grav = VectorXd::Zero(2 * m);
  double mass;
  for (uint i = 0; i < m; i++) {
    mass = this->M.coeff(2 * i, 2 * i);
    F_grav(2 * i + 1) = mass * g;
  } // for
  ResetLockedVertices(true);
} // SetGravity


// sets sprinForceConstant to C and also changes this
// in pVec for all SpringPotentials by changing their weights
void Sim2D::SetSpringForceConstant(double C) {
  this->springForceConstant = C;
  for (uint i = 0; i < pVec.size( ); i++)
    pVec[i].w = C;
  // system matrix has changed, so recompute lMatrix:
  InitLMatrix( );
} // SetSpringForceConstant

// computes lMatrix and initializes solver
void Sim2D::InitLMatrix( ) {
  //qDebug( ) << "Init lMatrix! h =" << h;

  // compute (constant) lMatrix
  lMatrix = SparseMatrix<double> (2 * m, 2 * m); // left side
  uint v1, v2; // indices of vertices
  //qDebug( ) << "lMatrix initialized, now adding S S^T";
  for (uint e = 0; e < pVec.size( ); e++) {
    v1 = pVec[e].v1;
    v2 = pVec[e].v2;

    // add w_i on diagonal, pos v1 and v2:
    lMatrix.coeffRef(2 * v1, 2 * v1) += 1 * pVec[e].w;
    lMatrix.coeffRef(2 * v1 + 1, 2 * v1 + 1) += 1 * pVec[e].w;
    lMatrix.coeffRef(2 * v2, 2 * v2) += 1 * pVec[e].w;
    lMatrix.coeffRef(2 * v2 + 1, 2 * v2 + 1) += 1 * pVec[e].w;

    // add -w_i on non-diagonal
    lMatrix.coeffRef(2 * v1, 2 * v2) -= 1 * pVec[e].w;
    lMatrix.coeffRef(2 * v1 + 1, 2 * v2 + 1) -= 1 * pVec[e].w;
    lMatrix.coeffRef(2 * v2, 2 * v1) -= 1 * pVec[e].w;
    lMatrix.coeffRef(2 * v2 + 1, 2 * v1 + 1) -= 1 * pVec[e].w;
  } // for

  //qDebug( ) << "adding S S^T done, adding M/h^2";

  if (h == 0)
    qDebug( ) << "Error in InitLMatrix, h = 0!";


  lMatrix += (1.0 / (h*h)) * this->M;

  time_t t = clock( );
  // init cholenskysolver:
  //qDebug( ) << "Done. now cholenskySolver.analyzePattern(lMatrix):";
  this->cholenskySolver.analyzePattern(lMatrix);
  //qDebug( ) << "cholenskySolver.factorize(lMatrix):";
  this->cholenskySolver.factorize(lMatrix); // now solver is ready to solve [lMatrix * x = b]
  double elapsed = double(clock( ) - t) / CLOCKS_PER_SEC;
  //qDebug( ) << "Done with Cholensky decomp, comp time =" << 1000 * elapsed << "ms";

  // init other solver
  this->cg.compute(lMatrix); // now solver is ready to solve [lMatrix * x = b]
} // InitLMatrix




// updates q and v by using local/global solve
// if useLLT == true then the LLT solver is used
// if doMeasures == true, then measurements are taken per iteration (takes time)
void Sim2D::NextStep(bool useLLT, bool doMeasures) {
  VectorXd oldQ; // old positions
  VectorXd prevQ; // compare positions between iterations

  if (simStep == 0)
    InitLMatrix( );

  uint numSteps = iterationsPerStep;
  uint index;

  oldQ = this->q;
  prevQ = this->q;

  double cur_qSolveTime = 0;
  double cur_rMatrixMakeTime = 0;
  double oldE = 0;

  if (doMeasures)
    oldE = this->GetTotEnergy( );

  this->STEP_disp.clear( );
  this->STEP_totDiffE.clear( );

  // for measuring time
  auto start = std::chrono::high_resolution_clock::now( );
  auto elapsed = std::chrono::high_resolution_clock::now( ) - start;
  long long microseconds;

  ResetLockedVertices( ); // to make v and F zero for locked vertices

  ComputeFloorForce( );
  this->F = F_grav + F_floor;
  VectorXd sn = this->q + (h * this->v) + ((h*h) * (Minv * F)); // estimate of new q

  for (uint step = 0; step < numSteps; step++) {
    // LOCAL: FIND pi's
    ComputePs( );

    // compute right:
    start = std::chrono::high_resolution_clock::now( );
    VectorXd rMatrix = VectorXd::Zero(2 * m); // right side


    for (uint i = 0; i < pVec.size( ); i++) {
      pVec[i].AddToPosVec(rMatrix);
    } // for

    rMatrix += (1.0 / (h*h)) * (this->M * sn);

    elapsed = std::chrono::high_resolution_clock::now( ) - start;
    microseconds = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count( );
    cur_rMatrixMakeTime += double(microseconds) / 1000;

    start = std::chrono::high_resolution_clock::now( );
    if (useLLT)
      q = this->cholenskySolver.solve(rMatrix);
    else
      q = this->cg.solve(rMatrix);

    elapsed = std::chrono::high_resolution_clock::now( ) - start;
    microseconds = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count( );
    cur_qSolveTime += double(microseconds) / 1000;

    // reset locked vertices positions
    for (uint i = 0; i < lockedVertices.size( ); i++) {
      index = lockedVertices[i];
      q(2 * index) = oldQ(2 * index);
      q(2 * index + 1) = oldQ(2 * index + 1);
    } // for

    if (doMeasures) {
      VectorXd qDiff = q - prevQ;
      this->v = (1.0 / h) * (q - oldQ);
      if (step > 0) {
        this->STEP_disp.push_back(qDiff.norm( ) / sqrt(qDiff.rows( )));
        this->STEP_totDiffE.push_back(fabs(GetTotEnergy( ) - oldE));
      } // if
      prevQ = q;
    } // if
  } // for



  this->v = (1.0 / h) * (q - oldQ);

  ResetLockedVertices(false);

  cur_qSolveTime /= numSteps;
  cur_rMatrixMakeTime /= numSteps;

  if (simStep > 0) { // average over previous values
    this->qSolveTime = 0.999 * this->qSolveTime + 0.001 * cur_qSolveTime;
    this->rMatrixMakeTime = 0.999 * this->rMatrixMakeTime + 0.001 * cur_rMatrixMakeTime;
  } // if
  else {
    this->qSolveTime = cur_qSolveTime;
    this->rMatrixMakeTime = cur_rMatrixMakeTime;
  } // else

  simStep++;
  timeInSim += h;
} // NextStep

// sets forces and velocity of locked vertices to 0,
// if [forcesToo] is false, then forces are not reset
void Sim2D::ResetLockedVertices(bool forcesToo) {
  uint index;
  for (uint i = 0; i < lockedVertices.size( ); i++) {
    index = lockedVertices[i];
    v(2 * index) = 0;
    v(2 * index + 1) = 0;
    if (forcesToo) {
      F(2 * index) = 0;
      F(2 * index + 1) = 0;
    } // if
  } // for
} // ResetLockedVertices



// initializes imgCenterX, imgCenterY and imgViewSize
void Sim2D::InitImgParams( ) {
  double minX = MinX( );
  double minY = MinY( );
  double maxX = MaxX( );
  double maxY = MaxY( );

//  qDebug( ) << "ImgParams, min/maxX =" << minX << maxX
//            << "min/maxY =" << minY << maxY;

  double W = maxX - minX;
  double H = maxY - minY;
  double L = max(W, H);

  // add border
  minX -= L / 6;
  minY -= L / 6;
  maxX += L / 6;
  maxY += L / 6;
  W = maxX - minX;
  H = maxY - minY;
  L = max(W, H);

  this->imgCenterX = 0.5 * (maxX + minX);
  this->imgCenterY = 0.5 * (maxY + minY);
  this->imgViewSize = L;
} // InitImgParams


// returns SIZE * SIZE image
QImage Sim2D::ToQImage(uint SIZE, bool useAA, bool drawEdges,
                       bool drawSelectedVertices, bool drawLockedVertices,
                       uint DRAW_MODE) {

  QImage img = QImage(SIZE, SIZE, QImage::Format_RGB888);
  QPainter painter(&img);
  QPen pen;

  painter.fillRect(0, 0, SIZE, SIZE, QColor(220, 220, 220));

  if (useAA)
    painter.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);

  if (imgViewSize == 0)
    InitImgParams( );

  double minX = imgCenterX - 0.5 * imgViewSize;
  double minY = imgCenterY - 0.5 * imgViewSize;
  double L = imgViewSize;

  // compute positions in image
  double x, y;
  vector <double> sx, sy;

  for (uint i = 0; i < m; i++) {
    x = this->q(2 * i) - minX;
    y = this->q(2 * i + 1) - minY;
    x *= SIZE / L; // in [0, SIZE]
    y *= SIZE / L; // in [0, SIZE]
    sx.push_back(x);
    sy.push_back(y);
  } // for

  painter.setBrush(QBrush(QColor(80, 200, 50)));
  painter.setPen(Qt::NoPen);

  // draw edges
  if (drawEdges) {
    uint v1, v2; // indices of vertices
    if (DRAW_MODE == 2) { // draw edge stress
      double cH, cL, cS; // hsl values
      double valHans; // velocity value in [0 1]
      double minU = MinU( ) - 1e-6;
      double maxU = MaxU( ) + 1e-6;
      double uRange = maxU - minU;

      for (uint e = 0; e < pVec.size( ); e++) {
        valHans = (GetU(e) - minU) / uRange; // in [0 1]

        if (valHans < 0)
          valHans = 0;
        if (valHans > 1)
          valHans = 1;

        cH = (1.0 - valHans);
        cS = 1.0;
        cL = valHans * 0.5;

        QColor col;
        col.setHslF(cH, cS, cL);
        pen.setColor(col);
        painter.setPen(pen);
        painter.setBrush(QBrush(col));
        v1 = pVec[e].v1;
        v2 = pVec[e].v2;
        painter.drawLine(QPointF(sx[v1], sy[v1]), QPointF(sx[v2], sy[v2]));
      } // for
    } // if
    else {
      pen.setColor(Qt::black);
      painter.setPen(pen);

      for (uint e = 0; e < numEdges; e++) {
        v1 = E(e, 0);
        v2 = E(e, 1);
        painter.drawLine(QPointF(sx[v1], sy[v1]), QPointF(sx[v2], sy[v2]));
      } // for
    } // else
  } // if

  // draw vertices
  painter.setBrush(QBrush(QColor(80, 200, 50)));
  painter.setPen(Qt::NoPen);

  if (DRAW_MODE == 0 || DRAW_MODE == 2) { // normal draw mode or edge draw mode
    for (uint i = 0; i < m; i++)
      painter.drawEllipse(QPointF(sx[i], sy[i]), 2, 2);
  } // if

  if (DRAW_MODE == 1) { // color vertices by velocity draw mode
    double cH, cL, cS; // hsl values
    double valHans; // velocity value in [0 1]
    double minV = MinV( ) - 1e-6;
    double maxV = MaxV( ) + 1e-6;
    double vRange = maxV - minV;

    for (uint i = 0; i < m; i++) {
      valHans = (GetV(i) - minV) / vRange; // in [0 1]

      cH = (1.0 - valHans);
      cS = 1.0;
      cL = valHans * 0.5;

      QColor col;
      col.setHslF(cH, cS, cL);
      pen.setColor(col);
      painter.setPen(pen);
      painter.setBrush(QBrush(col));
      painter.drawEllipse(QPointF(sx[i], sy[i]), 2, 2);
    } // for
  } // if

  if (drawSelectedVertices) {
    painter.setBrush(QBrush(QColor(255, 10, 10)));
    for (uint i = 0; i < this->selectedVertices.size( ); i++)
      painter.drawEllipse(QPointF(sx[selectedVertices[i]], sy[selectedVertices[i]]), 3, 3);
  } // if

  if (drawLockedVertices) {
    painter.setBrush(QBrush(QColor(10, 10, 255)));
    for (uint i = 0; i < this->lockedVertices.size( ); i++)
      painter.drawEllipse(QPointF(sx[lockedVertices[i]], sy[lockedVertices[i]]), 3, 3);
  } // if


  if (floorEnabled) { // draw floor
    // floor line
    double fY = floorHeight - minY;
    fY *= SIZE / L;
    pen.setColor(Qt::darkGreen);
    painter.setPen(pen);
    painter.drawLine(0, fY, SIZE, fY);
    // floor force line
    fY = floorHeight - floorDist - minY;
    fY *= SIZE / L;
    pen.setColor(Qt::darkYellow);
    painter.setPen(pen);
    painter.drawLine(0, fY, SIZE, fY);
  } // if

  return img;
} // ToQImage

QImage Sim2D::GetlMatrixImage( ) {
  uint SIZE = 2 * m;
  QImage img = QImage(SIZE, SIZE, QImage::Format_RGB888);
  QPainter painter(&img);
  painter.fillRect(0, 0, SIZE, SIZE, Qt::black);

  if (m == 0)
    return img;

  double maxVal = -1e99, minVal = 1e99, val;
  uint nVals = 0;

  for (int k = 0; k < lMatrix.outerSize( ); ++k) {
    for (SparseMatrix<double>::InnerIterator it(lMatrix, k); it; ++it) {
      val = it.value( );
      nVals++;
      if (val > maxVal)
        maxVal = val;
      if (val < minVal)
        minVal = val;
    } // for
  } // for

  double cv;
  QColor c;

  for (int k = 0; k < lMatrix.outerSize( ); ++k) {
    for (SparseMatrix<double>::InnerIterator it(lMatrix, k); it; ++it) {
      val = it.value( );
      if (val < 0) {
        cv = 255 * fabs(val) / fabs(minVal);
        c = QColor(0, 0, cv);
      } // if
      else {
        cv = 255 * fabs(val) / fabs(maxVal);
        c = QColor(cv, 0, 0);
      } // if

      img.setPixel(it.col( ), it.row( ), c.toRgb( ).rgb( ));
    } // for
  } // for
  return img;
} // GetlMatrixImage


// puts all vertices in given range in vector [selectedVertices]
// x1,x2,y1,y2 in range [0 1]
void Sim2D::SetSelectedVertices(double x1, double x2, double y1, double y2) {
  double minX = MinX( );
  double minY = MinY( );
  double maxX = MaxX( );
  double maxY = MaxY( );
  double W = maxX - minX;
  double H = maxY - minY;
  double x, y;

  this->selectedVertices.clear( );

  for (uint i = 0; i < m; i++) {
    x = this->q(2 * i);
    y = this->q(2 * i + 1);

    x -= minX;
    y -= minY;
    x /= W;
    y /= H;

    if (x >= x1 && x <= x2)
      if (y >= y1 && y <= y2)
        selectedVertices.push_back(i);
  } // for
} // SetSelectedVertices


// adds all vertices in [selectedVertices] to [lockedVertices]
void Sim2D::AddLockedVertices( ) {
  for (uint i = 0; i < selectedVertices.size( ); i++)
    lockedVertices.push_back(selectedVertices[i]);
} // AddLockedVertices
















