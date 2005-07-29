import java.io.DataInput;
import java.io.IOException;

public class BdescAutoRelease extends Opcode
{
	private final int block, ddesc, ref_count, ar_count, dd_count;
	
	public BdescAutoRelease(int block, int ddesc, int ref_count, int ar_count, int dd_count)
	{
		this.block = block;
		this.ddesc = ddesc;
		this.ref_count = ref_count;
		this.ar_count = ar_count;
		this.dd_count = dd_count;
	}
	
	public void applyTo(SystemState state)
	{
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_BDESC_AUTORELEASE, "KDB_BDESC_AUTORELEASE", BdescAutoRelease.class);
		factory.addParameter("block", 4);
		factory.addParameter("ddesc", 4);
		factory.addParameter("ref_count", 4);
		factory.addParameter("ar_count", 4);
		factory.addParameter("dd_count", 4);
		return factory;
	}
}
