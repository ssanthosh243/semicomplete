/**
 * BSShip.java
 *
 * Version:
 *   $Id$
 *
 * Revisions:
 *   $Log$
 *   Revision 1.8  2004/01/19 04:27:36  njohnson
 *   All stubs written. Time to test.
 *
 *   Revision 1.7  2004/01/19 02:06:04  njohnson
 *   fixed some javadoc mistakes
 *
 *   Revision 1.6  2004/01/19 00:35:46  njohnson
 *   Committing Compilable code. Added a getGridArray() funtion for grids
 *   which allows the array to be accessed directly for game grids.
 *
 *   Revision 1.5  2004/01/18 23:02:48  njohnson
 *   added some important stubs that need to be implemented.
 *
 *   Revision 1.4  2004/01/13 02:14:24  njohnson
 *   I finished the BSGrid's generateGrid() method which is now quite huge.
 *   It might be better with some AI( or worse ) since it just tries to throw
 *   ships down randomly and checks for overlaps.
 *   I also added a default constructor to BSShip to ease my mind.
 *
 *   Revision 1.3  2004/01/12 23:33:02  njohnson
 *   Almost completed with BSShip. Just need to finish commenting.
 *
 *   Revision 1.2  2004/01/12 04:51:42  njohnson
 *   I made the code compilation error free. Still mostly stubs.
 *
 *   Revision 1.1  2004/01/12 03:29:02  njohnson
 *   initial commit
 *
 *
 */

/**
 * A class that contains the endpoints of a ship in 
 * a BattleShip fleet
 *
 * @author Nicholas R. Johnson
 */
public class BSShip {
    private byte[] location;    
    private int length;

    /**
     *  A default constructor that makes a ship at A, 1.
     *
     *
     */
    public BSShip() {
	location = new byte[4];
	length = 0;
    } // constructor
    
    /**
     * Constructs a ship with specific coords.
     *
     * @param startPointX
     * @param startPointY
     * @param endPointX
     * @param endPointY
     */
    public BSShip( byte startPointX, 
            byte startPointY,        
            byte endPointX,
            byte endPointY ) {

        location = new byte[ 4 ];
        location[ 0 ] = startPointX;
        location[ 1 ] = startPointY;
        location[ 2 ] = endPointX;
        location[ 3 ] = endPointY;

        if( endPointX == startPointX ) {
            length = Math.abs( endPointY - startPointY ); 
	} else {
            length = Math.abs( endPointX - startPointX );
	}

    } // constructor

    /**
     * Returns a 2-byte byte array with
     * the starting point of the ship.
     * Format: { <row>, <column> }
     * Where <row> is the number associated with the letter of 
     * the grid's row ( 1 - A, 2 - B, ... , 9 - I, 10 - J )
     * and <column> is the number of the grid's column. 
     *
     * @return 
     */
    public byte[] getStart() {
        byte[] startLocation = new byte[2];    
        startLocation[ 0 ] = location[ 0 ];
        startLocation[ 1 ] = location[ 1 ];
        return startLocation;
    } // getStart()

    /**
     * Returns a 2-byte byte arrays with
     * the ending point of the ship.
     * Format: { <row>, <column> }
     * Where <row> is the number associated with the letter of 
     * the grid's row ( 1 - A, 2 - B, ... , 9 - I, 10 - J )
     * and <column> is the number of the grid's column. 
     *
     * @return
     */
    public byte[] getEnd() {
        byte[] endLocation = new byte[2];
        endLocation[ 0 ] = location[ 2 ];
        endLocation[ 1 ] = location[ 3 ];
        return endLocation;
    } //getEnd()

    /** 
     *
     *
     */
    public int getLength() {
        return length;
    }
} //BSShip


