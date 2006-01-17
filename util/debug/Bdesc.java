public class Bdesc
{
	public final int address;
	public final int ddesc;
	public final int number;
	
	public Bdesc(int address, int ddesc, int number)
	{
		this.address = address;
		this.ddesc = ddesc;
		this.number = number;
	}
	
	public String toString()
	{
		return "[bdesc " + SystemState.hex(address) + ": ddesc " + SystemState.hex(ddesc) + ": number " + number + "]";
	}
}
