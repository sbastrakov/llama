#include <iostream>
#include <utility>
#include <list>

#define ASYNCCOPY_CUDA 0
#define ASYNCCOPY_ASYNC 1
#define ASYNCCOPY_SHARED 1
#define ASYNCCOPY_LOCAL_SUM 1
#define ASYNCCOPY_SAVE 1
#define ASYNCCOPY_CHUNK_COUNT 4

#define ASYNCCOPY_DEFAULT_IMG_X 4096
#define ASYNCCOPY_DEFAULT_IMG_Y 4096
#define ASYNCCOPY_KERNEL_SIZE 8
#define ASYNCCOPY_CHUNK_SIZE 512
#define ASYNCCOPY_TOTAL_ELEMS_PER_BLOCK 16

#include <alpaka/alpaka.hpp>
#ifdef __CUDACC__
	#define LLAMA_FN_HOST_ACC_INLINE ALPAKA_FN_ACC __forceinline__
#else
	#define LLAMA_FN_HOST_ACC_INLINE ALPAKA_FN_ACC inline
#endif
#include <llama/llama.hpp>

using Element = float;

namespace px
{
    struct R {};
    struct G {};
    struct B {};
}

using Pixel = llama::DS<
    llama::DE< px::R, Element >,
    llama::DE< px::G, Element >,
    llama::DE< px::B, Element >
>;

using PixelOnAcc = llama::DS<
    //~ llama::DE< px::R, Element >,
    llama::DE< px::G, Element >,
    llama::DE< px::B, Element >
>;


#include <random>

// png++ has a bug on nvcc, which is fixed in this file
#include "solid_pixel_buffer_fixed.hpp"
#include <png++/png.hpp>

#include "../common/AlpakaAllocator.hpp"
#include "../common/AlpakaMemCopy.hpp"
#include "../common/AlpakaThreadElemsDistribution.hpp"
#include "../common/Chrono.hpp"

template<
    std::size_t elems,
    std::size_t kernelSize,
    std::size_t totalElemsPerBlock
>
struct BlurKernel
{
    template<
        typename T_Acc,
        typename T_View
    >
    LLAMA_FN_HOST_ACC_INLINE
    void operator()(
        T_Acc const &acc,
        T_View oldImage,
        T_View newImage
    ) const
    {
        constexpr auto threadsPerBlock = totalElemsPerBlock / elems;

        auto const threadIdxInGrid = alpaka::idx::getIdx<
            alpaka::Grid,
            alpaka::Threads
        >( acc );

#if ASYNCCOPY_SHARED == 1
        auto const threadIdxInBlock = alpaka::idx::getIdx<
            alpaka::Block,
            alpaka::Threads
        >( acc );
        auto const blockIndex = alpaka::idx::getIdx<
            alpaka::Grid,
            alpaka::Blocks
        >( acc );

        auto treeOperationList = llama::makeTuple(
            llama::mapping::tree::functor::LeafOnlyRT( )
        );
        using SharedMapping = llama::mapping::tree::Mapping<
            typename decltype(oldImage)::Mapping::UserDomain,
            typename decltype(oldImage)::Mapping::DatumDomain,
            decltype( treeOperationList )
        >;
        auto constexpr sharedChunkSize = totalElemsPerBlock + 2 * kernelSize;
        SharedMapping const sharedMapping(
            { sharedChunkSize, sharedChunkSize },
            treeOperationList
        );
        using SharedFactory = llama::Factory<
            SharedMapping,
            common::allocator::AlpakaShared<
                T_Acc,
                llama::SizeOf< PixelOnAcc >::value
                * sharedChunkSize * sharedChunkSize,
                __COUNTER__
            >
        >;
        auto shared = SharedFactory::allocView( sharedMapping, acc );

        std::size_t const b_start[2] = {
            blockIndex[0] * totalElemsPerBlock + threadIdxInBlock[0],
            blockIndex[1] * totalElemsPerBlock + threadIdxInBlock[1]
        };
        std::size_t const b_end[2] = {
            alpaka::math::min(
                acc,
                (blockIndex[0] + 1) * totalElemsPerBlock + 2 * kernelSize,
                oldImage.mapping.userDomainSize[0]
            ),
            alpaka::math::min(
                acc,
                (blockIndex[1] + 1) * totalElemsPerBlock + 2 * kernelSize,
                oldImage.mapping.userDomainSize[1]
            ),
        };
        LLAMA_INDEPENDENT_DATA
        for( auto y = b_start[0]; y < b_end[0]; y += threadsPerBlock )
            LLAMA_INDEPENDENT_DATA
            for( auto x = b_start[1]; x < b_end[1]; x += threadsPerBlock )
                shared(
                    y - blockIndex[0] * totalElemsPerBlock,
                    x - blockIndex[1] * totalElemsPerBlock
                ) = oldImage( y, x );
#endif // ASYNCCOPY_SHARED

        std::size_t const start[2] = {
            threadIdxInGrid[0] * elems,
            threadIdxInGrid[1] * elems
        };
        std::size_t const end[2] = {
            alpaka::math::min(
                acc,
                (threadIdxInGrid[0] + 1) * elems,
                oldImage.mapping.userDomainSize[0] - 2 * kernelSize
            ),
            alpaka::math::min(
                acc,
                (threadIdxInGrid[1] + 1) * elems,
                oldImage.mapping.userDomainSize[1] - 2 * kernelSize
            ),
        };

        LLAMA_INDEPENDENT_DATA
        for( auto y = start[0]; y < end[0]; ++y )
            LLAMA_INDEPENDENT_DATA
            for( auto x = start[1]; x < end[1]; ++x )
            {
#if ASYNCCOPY_LOCAL_SUM == 1
                auto sum = llama::tempAlloc< 1, PixelOnAcc >();
                sum() = 0;
#else // ASYNCCOPY_LOCAL_SUM == 0
                newImage( y + kernelSize, x + kernelSize ) = 0;
#endif // ASYNCCOPY_LOCAL_SUM

#if ASYNCCOPY_SHARED == 0
                std::size_t const i_start[2] = {
                    y,
                    x
                };
                std::size_t const i_end[2] = {
                    y + 2 * kernelSize + 1,
                    x + 2 * kernelSize + 1
                };
#else // ASYNCCOPY_SHARED == 1
                using ItType = long int;
                ItType const i_start[2] = {
                    ItType(y) - ItType(blockIndex[0] * totalElemsPerBlock),
                    ItType(x) - ItType(blockIndex[1] * totalElemsPerBlock)
                };
                ItType const i_end[2] = {
                    ItType(y + 2 * kernelSize + 1) - ItType(blockIndex[0] * totalElemsPerBlock),
                    ItType(x + 2 * kernelSize + 1) - ItType(blockIndex[1] * totalElemsPerBlock)
                };
#endif // ASYNCCOPY_SHARED
                LLAMA_INDEPENDENT_DATA
                for ( auto b = i_start[0]; b < i_end[0]; ++b )
                    LLAMA_INDEPENDENT_DATA
                    for ( auto a = i_start[1]; a < i_end[1]; ++a )
#if ASYNCCOPY_LOCAL_SUM == 1
                        sum() +=
#else // ASYNCCOPY_LOCAL_SUM == 0
                        newImage( y + kernelSize, x + kernelSize ) +=
#endif // ASYNCCOPY_LOCAL_SUM
#if ASYNCCOPY_SHARED == 0
                            oldImage( std::size_t(b), std::size_t(a) );
#else // ASYNCCOPY_SHARED == 1
                            shared( std::size_t(b), std::size_t(a) );
#endif // ASYNCCOPY_SHARED
#if ASYNCCOPY_LOCAL_SUM == 1
                sum() /= Element( (2 * kernelSize + 1) * (2 * kernelSize + 1) );
                newImage( y + kernelSize, x + kernelSize ) = sum();
#else // ASYNCCOPY_LOCAL_SUM == 0
                newImage( y + kernelSize, x + kernelSize ) /=
                    Element( (2 * kernelSize + 1) * (2 * kernelSize + 1) );
#endif // ASYNCCOPY_LOCAL_SUM
            }
    }
};

int main(int argc,char * * argv)
{
    constexpr std::size_t totalElemsPerBlock = ASYNCCOPY_TOTAL_ELEMS_PER_BLOCK;
    constexpr std::size_t chunkSize = ASYNCCOPY_CHUNK_SIZE;
    constexpr std::size_t chunkCount = ASYNCCOPY_CHUNK_COUNT;
    constexpr std::size_t kernelSize = ASYNCCOPY_KERNEL_SIZE;
    constexpr std::size_t hardwareThreads = 2; //relevant for OpenMP2Threads

    // ALPAKA
    using Dim = alpaka::dim::DimInt< 2 >;
    using Size = std::size_t;
    using Host = alpaka::acc::AccCpuSerial<Dim, Size>;

#if ASYNCCOPY_CUDA == 1
    using Acc = alpaka::acc::AccGpuCudaRt<Dim, Size>;
#if ASYNCCOPY_ASYNC == 1
    using Queue = alpaka::queue::QueueCudaRtAsync;
#else // ASYNCCOPY_ASYNC == 0
    using Queue = alpaka::queue::QueueCudaRtSync;
#endif // ASYNCCOPY_ASYNC
#else // ASYNCCOPY_CUDA == 0
    //~ using Acc = alpaka::acc::AccCpuSerial<Dim, Size>;
    using Acc = alpaka::acc::AccCpuOmp2Blocks<Dim, Size>;
    //~ using Acc = alpaka::acc::AccCpuOmp2Threads<Dim, Size>;
    //~ using Acc = alpaka::acc::AccCpuOmp4<Dim, Size>;
#if ASYNCCOPY_ASYNC == 1
    using Queue = alpaka::queue::QueueCpuAsync;
#else // ASYNCCOPY_ASYNC == 0
    using Queue = alpaka::queue::QueueCpuSync;
#endif // ASYNCCOPY_ASYNC
#endif // ASYNCCOPY_CUDA
    using DevHost = alpaka::dev::Dev<Host>;
    using DevAcc = alpaka::dev::Dev<Acc>;
    using PltfHost = alpaka::pltf::Pltf<DevHost>;
    using PltfAcc = alpaka::pltf::Pltf<DevAcc>;
    DevAcc const devAcc( alpaka::pltf::getDevByIdx< PltfAcc >( 0u ) );
    DevHost const devHost( alpaka::pltf::getDevByIdx< PltfHost >( 0u ) );
    std::vector< Queue > queue;
    for (std::size_t i = 0; i < chunkCount; ++i)
        queue.push_back( Queue( devAcc ) ) ;

    // ASYNCCOPY
    std::size_t img_x = ASYNCCOPY_DEFAULT_IMG_X;
    std::size_t img_y = ASYNCCOPY_DEFAULT_IMG_Y;
    std::size_t buffer_x = ASYNCCOPY_DEFAULT_IMG_X + 2 * kernelSize;
    std::size_t buffer_y = ASYNCCOPY_DEFAULT_IMG_Y + 2 * kernelSize;
    using Distribution = ThreadsElemsDistribution<
        Acc,
        totalElemsPerBlock,
        hardwareThreads
    >;
    constexpr std::size_t elemCount = Distribution::elemCount;
    constexpr std::size_t threadCount = Distribution::threadCount;

    //png::image< png::rgb_pixel > image;
    png::image< png::rgb_pixel >* image = NULL;
    std::string out_filename = "output.png";

    if (argc > 1)
    {
        image = new png::image< png::rgb_pixel >( argv[1] );
        img_x = image->get_width();
        img_y = image->get_height();
        buffer_x = image->get_width() + 2 * kernelSize;
        buffer_y = image->get_height() + 2 * kernelSize;
        if (argc > 2)
            out_filename = std::string( argv[2] );
    }

    // LLAMA
    using UserDomain = llama::UserDomain< 2 >;
    const UserDomain userDomainSize{ buffer_y, buffer_x };
    const UserDomain chunkUserDomain{
        chunkSize + 2 * kernelSize,
        chunkSize + 2 * kernelSize
    };

    auto treeOperationList = llama::makeTuple(
        llama::mapping::tree::functor::LeafOnlyRT( )
        //~ llama::mapping::tree::functor::Idem( )
    );
    using HostMapping = llama::mapping::tree::Mapping<
        UserDomain,
        Pixel,
        decltype( treeOperationList )
    >;
    using DevMapping = llama::mapping::tree::Mapping<
        UserDomain,
        PixelOnAcc,
        decltype( treeOperationList )
    >;
    const HostMapping hostMapping(
        userDomainSize,
        treeOperationList
    );
    const DevMapping devMapping(
        chunkUserDomain,
        treeOperationList
    );

    using HostFactory = llama::Factory<
        HostMapping,
        common::allocator::Alpaka<
            DevHost,
            Size
        >
    >;
    using HostChunkFactory = llama::Factory<
        DevMapping,
        common::allocator::Alpaka<
            DevHost,
            Size
        >
    >;
    using DevFactory = llama::Factory<
        DevMapping,
        common::allocator::Alpaka<
            DevAcc,
            Size
        >
    >;
    using MirrorFactory = llama::Factory<
        DevMapping,
        common::allocator::AlpakaMirror<
            DevAcc,
            Size,
            DevMapping
        >
    >;

    std::cout << "Image size: " << img_x << ":" << img_y << '\n';
    std::cout
        << hostMapping.getBlobSize(0) * 2 / 1024 / 1024
        << " MB on device\n";

    Chrono chrono;

    auto host = HostFactory::allocView( hostMapping, devHost );
    std::vector<
        decltype( HostChunkFactory::allocView( devMapping, devHost ) )
    > hostChunk;
    std::vector<
        decltype( DevFactory::allocView( devMapping,  devAcc ) )
    > devOld;
    decltype( devOld ) devNew;
    std::vector<
        decltype( MirrorFactory::allocView( devMapping,  devOld[0] ) )
    > mirrorOld;
    decltype( mirrorOld ) mirrorNew;
    for (std::size_t i = 0; i < chunkCount; ++i)
    {
        hostChunk.push_back(
            HostChunkFactory::allocView( devMapping, devHost )
        );
        devOld.push_back(
            DevFactory::allocView( devMapping,  devAcc )
        );
        devNew.push_back(
            DevFactory::allocView( devMapping,  devAcc )
        );
        mirrorOld.push_back(
            MirrorFactory::allocView( devMapping,  devOld[ i ] )
        );
        mirrorNew.push_back(
            MirrorFactory::allocView( devMapping,  devNew[ i ] )
        );
    }


    chrono.printAndReset("Alloc");

    if ( !image )
    {
        image = new png::image< png::rgb_pixel >( img_x, img_y );
        std::default_random_engine generator;
        std::normal_distribution< Element > distribution(
            Element( 0 ), // mean
            Element( 0.5 )  // stddev
        );
        LLAMA_INDEPENDENT_DATA
        for (std::size_t y = 0; y < buffer_y; ++y)
            for (std::size_t x = 0; x < buffer_x; ++x)
            {
                host( y, x )( px::R() ) = fabs(distribution(generator));
                host( y, x )( px::G() ) = fabs(distribution(generator));
                host( y, x )( px::B() ) = fabs(distribution(generator));
            }
    }
    else
    {
        LLAMA_INDEPENDENT_DATA
        for (std::size_t y = 0; y < buffer_y; ++y)
            for (std::size_t x = 0; x < buffer_x; ++x)
            {
                auto X = x;
                auto Y = y;
                if ( X < kernelSize )
                    X = kernelSize;
                if ( Y < kernelSize )
                    Y = kernelSize;
                if ( X > img_x + kernelSize - 1 )
                    X = img_x + kernelSize - 1;
                if ( Y > img_y + kernelSize - 1 )
                    Y = img_y + kernelSize - 1;
                host( y, x )( px::R() ) = Element((*image)
                    [ Y - kernelSize ]
                    [ X - kernelSize ].red) / 255.;
                host( y, x )( px::G() ) = Element((*image)
                    [ Y - kernelSize ]
                    [ X - kernelSize ].blue) / 255.;
                host( y, x )( px::B() ) = Element((*image)
                    [ Y - kernelSize ]
                    [ X - kernelSize ].green) / 255.;
            }
    }

    chrono.printAndReset("Init");
    const alpaka::vec::Vec< Dim, Size > elems (
        static_cast< Size >( elemCount ),
        static_cast< Size >( elemCount )
    );
    const alpaka::vec::Vec< Dim, Size > threads (
        static_cast< Size >( threadCount ),
        static_cast< Size >( threadCount )
    );
    const alpaka::vec::Vec< Dim, Size > blocks (
        static_cast< Size >( ( chunkSize + totalElemsPerBlock - 1 ) / totalElemsPerBlock ),
        static_cast< Size >( ( chunkSize + totalElemsPerBlock - 1 ) / totalElemsPerBlock )
    );
    const alpaka::vec::Vec< Dim, Size > chunks (
        static_cast< Size >( ( img_y + chunkSize - 1 ) / chunkSize ),
        static_cast< Size >( ( img_x + chunkSize - 1 ) / chunkSize )
    );

    auto const workdiv = alpaka::workdiv::WorkDivMembers<
        Dim,
        Size
    > {
        blocks,
        threads,
        elems
    };

    using HostViewType = llama::VirtualView< decltype( host ) >;
    struct VirtualHostElement
    {
        HostViewType virtualHost;
        const UserDomain validMiniSize;
    };
    std::list< VirtualHostElement > virtualHostList;
    for (std::size_t chunk_y = 0; chunk_y < chunks[0]; ++chunk_y)
        for (std::size_t chunk_x = 0; chunk_x < chunks[1]; ++chunk_x)
        {
            // Create virtual view with size of mini view
            const UserDomain validMiniSize{
                ((chunk_y < chunks[0] - 1) ?
                    chunkSize : (img_y - 1) % chunkSize + 1) + 2 * kernelSize,
                ((chunk_x < chunks[1] - 1) ?
                    chunkSize : (img_x - 1) % chunkSize + 1) + 2 * kernelSize
            };
            llama::VirtualView< decltype( host ) > virtualHost(
                host,
                {
                    chunk_y * chunkSize,
                    chunk_x * chunkSize
                },
                validMiniSize
            );
            // Find free chunk stream
            std::size_t chunkNr = virtualHostList.size();
            if (virtualHostList.size() < chunkCount)
                virtualHostList.push_back( {
                    virtualHost,
                    validMiniSize
                } );
            else
            {
                bool not_found = true;
                while (not_found)
                {
                    auto chunkIt = virtualHostList.begin();
                    for (chunkNr = 0; chunkNr < chunkCount; ++chunkNr)
                    {
                        if (alpaka::queue::empty( queue[chunkNr] ))
                        {
                            // Copy data back
                            LLAMA_INDEPENDENT_DATA
                            for (std::size_t y = 0; y < (*chunkIt).validMiniSize[0] - 2 * kernelSize; ++y)
                            {
                                LLAMA_INDEPENDENT_DATA
                                for (std::size_t x = 0; x < (*chunkIt).validMiniSize[1] - 2 * kernelSize; ++x)
                                    (*chunkIt).virtualHost(
                                        y + kernelSize,
                                        x + kernelSize
                                    ) = hostChunk[chunkNr](
                                        y + kernelSize,
                                        x + kernelSize
                                    );
                            }
                            chunkIt = virtualHostList.erase( chunkIt );
                            virtualHostList.insert(
                                chunkIt,
                                {
                                    virtualHost,
                                    validMiniSize
                                }
                            );
                            not_found = false;
                            break;
                        }
                        chunkIt++;
                    }
                    if (not_found)
                        usleep(1);
                }
            }
            //~ // Copy data from virtual view to mini view
            LLAMA_INDEPENDENT_DATA
            for (std::size_t y = 0; y < validMiniSize[0]; ++y)
            {
                LLAMA_INDEPENDENT_DATA
                for (std::size_t x = 0; x < validMiniSize[1]; ++x)
                    hostChunk[chunkNr]( y, x ) = virtualHost( y, x );
            }
            alpakaMemCopy( devOld[chunkNr], hostChunk[chunkNr], chunkUserDomain, queue[chunkNr] );
            BlurKernel<
                elemCount,
                kernelSize,
                totalElemsPerBlock
            > blurKernel;
            alpaka::kernel::exec<Acc>(
                queue[chunkNr],
                workdiv,
                blurKernel,
                mirrorOld[chunkNr],
                mirrorNew[chunkNr]
            );
            alpakaMemCopy( hostChunk[chunkNr], devNew[chunkNr], userDomainSize, queue[chunkNr] );
        }
    //Wait for not finished tasks on accelerator
    auto chunkIt = virtualHostList.begin();
    for (std::size_t chunkNr = 0; chunkNr < chunkCount; ++chunkNr)
    {
        alpaka::wait::wait( queue[chunkNr] );
        // Copy data back
        LLAMA_INDEPENDENT_DATA
        for (std::size_t y = 0; y < (*chunkIt).validMiniSize[0] - 2 * kernelSize; ++y)
        {
            LLAMA_INDEPENDENT_DATA
            for (std::size_t x = 0; x < (*chunkIt).validMiniSize[1] - 2 * kernelSize; ++x)
                (*chunkIt).virtualHost(
                    y + kernelSize,
                    x + kernelSize
                ) = hostChunk[chunkNr](
                    y + kernelSize,
                    x + kernelSize
                );
        }
        chunkIt++;
    }
    chrono.printAndReset("Blur kernel");

#if ASYNCCOPY_SAVE == 1
    LLAMA_INDEPENDENT_DATA
    for (std::size_t y = 0; y < img_y; ++y)
        for (std::size_t x = 0; x < img_x; ++x)
        {
            (*image)[ y ][ x ].red = host(
                y + kernelSize,
                x + kernelSize
            )( px::R() ) * 255.;
            (*image)[ y ][ x ].blue = host(
                y + kernelSize,
                x + kernelSize
            )( px::G() ) * 255.;
            (*image)[ y ][ x ].green = host(
                y + kernelSize,
                x + kernelSize
            )( px::B() ) * 255.;
        }
    image->write( out_filename );
#endif // ASYNCCOPY_SAVE

    delete(image);

    return 0;
}
