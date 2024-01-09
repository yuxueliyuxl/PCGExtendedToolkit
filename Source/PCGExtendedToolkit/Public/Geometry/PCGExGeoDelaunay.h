﻿// Copyright Timothé Lapetite 2023
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "PCGExGeo.h"
#include "PCGExGeoHull.h"
#include "PCGExGeoPrimtives.h"

namespace PCGExGeo
{
	template <int DIMENSIONS>
	class PCGEXTENDEDTOOLKIT_API TDelaunayCell
	{
	public:
		TFSimplex<DIMENSIONS>* Simplex = nullptr;
		TFVtx<DIMENSIONS>* Circumcenter = nullptr;
		double Radius = 0;

		TDelaunayCell(
			TFSimplex<DIMENSIONS>* InSimplex,
			TFVtx<DIMENSIONS>* InCircumcenter,
			const double InRadius)
		{
			Simplex = InSimplex;
			Circumcenter = InCircumcenter;
			Radius = InRadius;
		}

		~TDelaunayCell()
		{
			Simplex = nullptr;
			PCGEX_DELETE(Circumcenter)
			Radius = 0;
		}
	};

	template <int DIMENSIONS>
	class PCGEXTENDEDTOOLKIT_API TDelaunayTriangulation
	{
		bool bOwnsVertices = true;

	public:
		TConvexHull<DIMENSIONS>* Hull = nullptr;
		TArray<TFVtx<DIMENSIONS>*> Vertices;
		TArray<TDelaunayCell<DIMENSIONS>*> Cells;
		TFVtx<DIMENSIONS>* Centroid = nullptr;

		double MTX[DIMENSIONS][DIMENSIONS];

		TDelaunayTriangulation()
		{
		}

		virtual ~TDelaunayTriangulation()
		{
			PCGEX_DELETE_TARRAY(Cells)
			if (bOwnsVertices) { PCGEX_DELETE_TARRAY(Vertices) }
			else { Vertices.Empty(); }
			PCGEX_DELETE(Centroid)
			PCGEX_DELETE(Hull)
		}

		bool PrepareFrom(const TArray<FPCGPoint>& InPoints)
		{
			bOwnsVertices = true;

			PCGEX_DELETE_TARRAY(Cells)
			PCGEX_DELETE(Centroid)
			PCGEX_DELETE(Hull)

			Centroid = new TFVtx<DIMENSIONS>();
			GetUpscaledVerticesFromPoints<DIMENSIONS>(InPoints, Vertices);

			if (Vertices.Num() <= DIMENSIONS) { return false; }

			return true;
		}

		bool PrepareFrom(const TArray<TFVtx<DIMENSIONS>*>& InVertices)
		{
			bOwnsVertices = false;

			PCGEX_DELETE_TARRAY(Cells)
			PCGEX_DELETE(Centroid)
			PCGEX_DELETE(Hull)

			Centroid = new TFVtx<DIMENSIONS>();
			if (InVertices.Num() <= DIMENSIONS) { return false; }

			Vertices.Empty();
			Vertices.Reserve(InVertices.Num());
			Vertices.Append(InVertices);

			return true;
		}

		virtual void Generate()
		{
			PCGEX_DELETE(Hull)
			Hull = new TConvexHull<DIMENSIONS>();
			Hull->Generate(Vertices);

			for (int i = 0; i < DIMENSIONS; i++) { (*Centroid)[i] = Hull->Centroid[i]; }

			int i = 0;
			for (TFSimplex<DIMENSIONS>* Simplex : Hull->Simplices)
			{
				if (Simplex->Normal[DIMENSIONS - 1] >= 0.0f)
				{
					for (TFSimplex<DIMENSIONS>* Adjacent : Simplex->AdjacentFaces)
					{
						if (Adjacent) { Adjacent->Remove(Simplex); }
					}
				}
				else
				{
					TDelaunayCell<DIMENSIONS>* Cell = CreateCell(Simplex);
					Cell->Circumcenter->Id = i++;
					Cells.Add(Cell);
				}
			}
		}

		void GetUniqueEdges(TArray<PCGExGraph::FUnsignedEdge>& OutEdges)
		{
			TSet<uint64> UniqueEdges;
			UniqueEdges.Reserve(Cells.Num() * 3);

			for (const TDelaunayCell<DIMENSIONS>* Cell : Cells)
			{
				for (int i = 0; i < DIMENSIONS; i++)
				{
					const int32 A = Cell->Simplex->Vertices[i]->Id;
					for (int j = i + 1; j < DIMENSIONS; j++)
					{
						const int32 B = Cell->Simplex->Vertices[j]->Id;
						if (const uint64 Hash = PCGExGraph::GetUnsignedHash64(A, B);
							!UniqueEdges.Contains(Hash))
						{
							OutEdges.Emplace(A, B);
							UniqueEdges.Add(Hash);
						}
					}
				}
			}

			UniqueEdges.Empty();
		}

	protected:
		virtual TDelaunayCell<DIMENSIONS>* CreateCell(TFSimplex<DIMENSIONS>* Simplex) = 0;
		virtual double Determinant() const = 0;
	};

	class PCGEXTENDEDTOOLKIT_API TDelaunayTriangulation2 : public TDelaunayTriangulation<3>
	{
	public:
		TDelaunayTriangulation2() : TDelaunayTriangulation()
		{
		}

	protected:
		virtual double Determinant() const override
		{
			const double F00 = MTX[1][1] * MTX[2][2] - MTX[1][2] * MTX[2][1];
			const double F10 = MTX[1][2] * MTX[2][0] - MTX[1][0] * MTX[2][2];
			const double F20 = MTX[1][0] * MTX[2][1] - MTX[1][1] * MTX[2][0];
			return MTX[0][0] * F00 + MTX[0][1] * F10 + MTX[0][2] * F20;
		}

		virtual TDelaunayCell<3>* CreateCell(TFSimplex<3>* Simplex) override
		{
			// From MathWorld: http://mathworld.wolfram.com/Circumcircle.html

			// x, y, 1
			for (int i = 0; i < 3; i++)
			{
				TFVtx<3>& V = (*Simplex->Vertices[i]);
				MTX[i][0] = V[0];
				MTX[i][1] = V[1];
				MTX[i][2] = 1;
			}

			const double a = Determinant();

			// size, y, 1
			for (int i = 0; i < 3; i++) { MTX[i][0] = (*Simplex->Vertices[i])[2]; } //->SqrMagnitude();
			const double DX = -Determinant();

			// size, x, 1
			for (int i = 0; i < 3; i++) { MTX[i][1] = (*Simplex->Vertices[i])[0]; }
			const double DY = Determinant();

			// size, x, y
			for (int i = 0; i < 3; i++) { MTX[i][2] = (*Simplex->Vertices[i])[1]; }
			const double c = -Determinant();

			const double s = -1.0f / (2.0f * a);

			TFVtx<3>* CC = new TFVtx<3>();
			CC->SetV3(FVector(s * DX, s * DY, 0));

			return new TDelaunayCell(
				Simplex, CC,
				FMath::Abs(s) * FMath::Sqrt(DX * DX + DY * DY - 4.0 * a * c));
		}
	};

	class PCGEXTENDEDTOOLKIT_API TDelaunayTriangulation3 : public TDelaunayTriangulation<4>
	{
	public:
		TDelaunayTriangulation3() : TDelaunayTriangulation()
		{
		}

	protected:
		double MINOR(const int R0, const int R1, const int R2, const int C0, const int C1, const int C2) const
		{
			return
				MTX[R0][C0] * (MTX[R1][C1] * MTX[R2][C2] - MTX[R2][C1] * MTX[R1][C2]) -
				MTX[R0][C1] * (MTX[R1][C0] * MTX[R2][C2] - MTX[R2][C0] * MTX[R1][C2]) +
				MTX[R0][C2] * (MTX[R1][C0] * MTX[R2][C1] - MTX[R2][C0] * MTX[R1][C1]);
		}

		virtual double Determinant() const override
		{
			return (MTX[0][0] * MINOR(1, 2, 3, 1, 2, 3) -
				MTX[0][1] * MINOR(1, 2, 3, 0, 2, 3) +
				MTX[0][2] * MINOR(1, 2, 3, 0, 1, 3) -
				MTX[0][3] * MINOR(1, 2, 3, 0, 1, 2));
		}

		virtual TDelaunayCell<4>* CreateCell(TFSimplex<4>* Simplex) override
		{
			// From MathWorld: http://mathworld.wolfram.com/Circumsphere.html

			// x, y, z, 1
			for (int i = 0; i < 4; i++)
			{
				TFVtx<4>& V = (*Simplex->Vertices[i]);
				MTX[i][0] = V[0];
				MTX[i][1] = V[1];
				MTX[i][2] = V[2];
				MTX[i][3] = 1;
			}
			const double a = Determinant();

			// size, y, z, 1
			for (int i = 0; i < 4; i++) { MTX[i][0] = (*Simplex->Vertices[i])[3]; } //->SqrMagnitude();
			const double DX = Determinant();

			// size, x, z, 1
			for (int i = 0; i < 4; i++) { MTX[i][1] = (*Simplex->Vertices[i])[0]; }
			const double DY = -Determinant();

			// size, x, y, 1
			for (int i = 0; i < 4; i++) { MTX[i][2] = (*Simplex->Vertices[i])[1]; }
			const double DZ = Determinant();

			//size, x, y, z
			for (int i = 0; i < 4; i++) { MTX[i][3] = (*Simplex->Vertices[i])[2]; }
			const double c = Determinant();

			const double s = -1.0f / (2.0f * a);

			TFVtx<4>* CC = new TFVtx<4>();
			CC->SetV4(FVector4(s * DX, s * DY, s * DZ, 0));

			return new TDelaunayCell(
				Simplex, CC,
				FMath::Abs(s) * FMath::Sqrt(DX * DX + DY * DY + DZ * DZ - 4 * a * c));
		}
	};
}
