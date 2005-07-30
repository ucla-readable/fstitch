public class Bdesc
{
	public final int address;
	public final int ddesc;
	
	public Bdesc(int address, int ddesc)
	{
		this.address = address;
		this.ddesc = ddesc;
	}
	
	public String toString()
	{
		return "[bdesc " + SystemState.hex(address) + ": ddesc " + SystemState.hex(ddesc) + "]";
	}
}
