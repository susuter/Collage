
/* Copyright (c) 2009-2010, Stefan Eilemann <eile@equalizergraphics.com> 
 *
 * This library is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License version 2.1 as published
 * by the Free Software Foundation.
 *  
 * This library is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for more
 * details.
 * 
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifndef EQFABRIC_OBSERVER_H
#define EQFABRIC_OBSERVER_H

#include <eq/fabric/object.h>         // base class
#include <eq/fabric/types.h>
#include <eq/fabric/visitorResult.h> // enum

#include <string>
#include <vector>

namespace eq
{
namespace fabric
{
    template< typename T > class LeafVisitor;
    struct ObserverPath;

    /**
     * An Observer looks at one or more views from a certain position (head
     * matrix) with a given eye separation. Multiple observers in a
     * configuration can be used to update independent viewers from one
     * configuration, e.g., a control host, a HMD and a Cave.
     */
    template< typename C, typename O > class Observer : public Object
    {
    public:
        typedef std::vector< O* > ObserverVector;

        /** @name Data Access */
        //@{
        /** @return the parent config of this observer. @version 1.0 */
        const C* getConfig() const { return _config; }

        /** @return the parent config of this observer. @version 1.0 */
        C* getConfig() { return _config; }

        /** @return the index path to this observer. @internal */
        ObserverPath getPath() const;

        /** Set the eye separation of this observer. @version 1.0 */
        EQFABRIC_EXPORT void setEyeBase( const float eyeBase );

        /** @return the current eye separation. @version 1.0 */
        float getEyeBase() const { return _eyeBase; }

        /** 
         * Set the head matrix.
         *
         * The head matrix specifies the transformation origin->observer.
         * Together with the eye separation, this determines the eye positions
         * in the global coordinate system. The eye position and wall or
         * projection description define the shape of the frustum and the
         * channel's head transformation during rendering.
         *
         * @param matrix the matrix
         * @version 1.0
         */
        EQFABRIC_EXPORT void setHeadMatrix( const Matrix4f& matrix );

        /** @return the current head matrix. @version 1.0 */
        const Matrix4f& getHeadMatrix() const { return _headMatrix; }
        //@}

        /** @name Operations */
        //@{
        /** 
         * Traverse this observer using a observer visitor.
         * 
         * @param visitor the visitor.
         * @return the result of the visitor traversal.
         * @version 1.0
         */
        EQFABRIC_EXPORT VisitorResult accept( LeafVisitor< O >& visitor );

        /** Const-version of accept(). @version 1.0 */
        EQFABRIC_EXPORT VisitorResult accept( LeafVisitor< O >& visitor ) const;
        //@}
        
    protected:
        /** Construct a new Observer. @internal */
        EQFABRIC_EXPORT Observer( C* config );

        /** Construct a new copy of the observer. @internal */
        Observer( const O& from, C* config );

        /** Destruct this observer. @internal */
        EQFABRIC_EXPORT virtual ~Observer();

        /** @sa Object::serialize */
        virtual void serialize( net::DataOStream& os,
                                const uint64_t dirtyBits );
        /** @sa Object::deserialize */
        virtual void deserialize( net::DataIStream& is,
                                  const uint64_t dirtyBits );

        enum DirtyBits
        {
            DIRTY_EYE_BASE   = Object::DIRTY_CUSTOM << 0,
            DIRTY_HEAD       = Object::DIRTY_CUSTOM << 1
        };

    private:
        /** The parent Config. */
        C* const _config;

        /** The current eye separation. */
        float _eyeBase;

        /** The current head position. */
        Matrix4f _headMatrix;

        union // placeholder for binary-compatible changes
        {
            char dummy[32];
        };
    };

    template< typename C, typename O >
    EQFABRIC_EXPORT std::ostream& operator << ( std::ostream&,
                                                const Observer< C, O >& );
}
}
#endif // EQFABRIC_OBSERVER_H
