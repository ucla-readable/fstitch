public class Bdesc
{
	public final int address;
	public final int ddesc;
	public final int number;
	public final short count;
	
	public Bdesc(int address, int ddesc, int number, short count)
	{
		this.address = address;
		this.ddesc = ddesc;
		this.number = number;
		this.count = count;
	}
	
	public String toString()
	{
		return "[bdesc " + SystemState.hex(address) + ": ddesc " + SystemState.hex(ddesc) + ": number " + number + ", count " + count + "]";
	}
}
